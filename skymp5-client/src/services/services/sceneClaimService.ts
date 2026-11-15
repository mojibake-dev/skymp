import { SceneActionEvent, SceneStartEvent } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { CLAIM_SCENE, SceneMarks, claimMessage, claimOutbox, isRecordId } from "./claimRules";

const TAG = "verb:npc-talk";
// the poll, PuppetService's
const POLL_MS = 500;
const HELD_LINES_MAX = 256;

const hex = (n: number) => n.toString(16);

// thuum docs/DECISIONS.md ADR-073 step 5a, the client half (decision 6 with
// D-L2 B, "a scene row owned by the seat that started it, claiming its actors
// for its life and surviving a grant", and D-A2 C, "the stage's cause seat
// owns a scene a fragment starts"). R-V1 (lab/.cache/drafts/reads-53): the
// ritual scene 5dba7 that Ariadne's seat started from rotfern's relayed stage
// was stopped there by the puppet rule, Falion a puppet, and its end fragment
// set the quest's stage.
//
// A scene is marked this seat's own when the platform raises sceneStart with
// no acting player (a Scene.Start or ForceStart this seat's own scripts
// called: ScriptNatives.cpp's NoteSceneStart, the dispatch's two natives
// alone, rule 9) or when its engine raises sceneAction for an actor this seat
// simulates (a scene the engine started with no Papyrus call; the event has
// no native filter, a scene's actions seconds apart, the package events'
// accepted cost). A sceneStart inside another player's acting scope marks the
// scene the cause seat's instead (D-A2 C): this seat's copy of a relayed
// fragment's scene, never claimed, and never taken for this seat's own by its
// actions on the actors this seat still simulates (x-scene-claim's leg 2,
// where the guest hosts). At each poll (500 ms, while a scene is marked)
// every high-process actor whose current scene is an own mark playing here is
// claimed once, ActorClaim {purpose 2, quest (the scene's owning quest),
// scene}, through the claims' outbox; the server checks the scene's own PNAM
// and its actor aliases, and the first claim wins. When the scene stops
// playing each claimed actor is released; a marked scene never seen playing
// within 10 s is let go. A new connection forgets the claims (the server
// ended them at the departure) and the poll asks again for a scene still
// playing.
//
// PuppetService asks holdsScene before it stops a scene its engine plays on a
// puppet: an owner never stops its own scene while its claim of the actor is
// unsent or unanswered (3 s); the word then promotes the actor here, or the
// claim did not take and the copy is stopped as before. A claim of an actor
// this seat simulated when it was sent holds nothing once she is a puppet
// here: another seat's claim won her. A puppet seat keeps rendering and stops
// only its own copy.
//
// HYPOTHESIS H-S1: the platform's dispatch sees a fragment's Scene.Start
// (NoteSceneStart). H-S2: getCurrentScene names every scene actor within one
// poll, a puppet here (its AI off) included. H-S3: a scene survives its
// actors being promoted as it starts. H-S4: the sceneStart tick task lands
// before PuppetService's next poll would stop the scene (a stop before it is
// the playtest 41 rule, as before) and before the scene's first action here
// (an action first would mark a relayed fragment's copy this seat's own).
export class SceneClaimService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
        this.controller.on("sceneAction", (e) => this.onSceneAction(e));
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        try {
            this.controller.on("sceneStart", (e) => this.onSceneStart(e));
        } catch (e) {
            logLab(TAG, `sceneStart unavailable on this platform (${e}); a scene is this seat's own by an action on an actor it simulates alone`);
        }
    }

    // PuppetService's ask (stopLocalScene): whether this seat started the
    // scene and its claim of the actor is still unsent or unanswered
    holdsScene(localId: number, sceneId: number): boolean {
        if (!this.marks.holdsStop(localId, sceneId, Date.now())) {
            return false;
        }
        const key = `${localId}:${sceneId}`;
        if (!this.heldLines.has(key)) {
            if (this.heldLines.size >= HELD_LINES_MAX) {
                this.heldLines.clear();
            }
            this.heldLines.add(key);
            logTrace(this, `scene ${hex(sceneId)} left playing on puppet ${hex(localId)}: this seat started it (${this.marks.how(sceneId)}) and its claim is pending`);
        }
        return true;
    }

    // the platform's tick task: JS state alone, no game call
    private onSceneStart(e: SceneStartEvent): void {
        if (!e || !isRecordId(e.sceneId)) {
            return;
        }
        const native = e.forced ? "Scene.ForceStart" : "Scene.Start";
        const acting = typeof e.acting === "number" ? e.acting : 0;
        if (acting !== 0) {
            if (this.marks.mark(e.sceneId, Date.now(), `${native} inside acting player ${hex(acting)}'s scope`, false)) {
                logLab(TAG, `scene ${hex(e.sceneId)} started here by ${native} inside acting player ${hex(acting)}'s scope: that seat's (D-A2 C), nothing claimed here`);
                this.nextPoll = 0;
            }
            return;
        }
        if (this.marks.mark(e.sceneId, Date.now(), native, true)) {
            logLab(TAG, `scene ${hex(e.sceneId)} started here by ${native}: this seat's own (D-L2 B), its actors claimed at the next poll`);
            this.nextPoll = 0;
        }
    }

    // the engine's scene action, an update task: the action's actor is its
    // alias's reference in its quest
    private onSceneAction(e: SceneActionEvent): void {
        const scene = e ? e.scene : null;
        const quest = e ? e.quest : null;
        if (!scene || !quest) {
            return;
        }
        const sceneId = scene.getFormID();
        if (this.marks.has(sceneId) || !isRecordId(sceneId)) {
            return;
        }
        const alias = this.sp.ReferenceAlias.from(quest.getAliasById(e.actorAliasId));
        const ref = alias ? alias.getReference() : null;
        const localId = ref ? ref.getFormID() : 0;
        if (localId === 0 || localId === 0x14) {
            return;
        }
        if (!this.controller.lookupListener(HostService).isMineLocal(localId)) {
            return;
        }
        const how = `an action on ${hex(localId)}, which this seat simulates`;
        if (this.marks.mark(sceneId, Date.now(), how, true)) {
            logLab(TAG, `scene ${hex(sceneId)} of ${hex(quest.getFormID())} plays here with ${how}: this seat's own (D-L2 B), its actors claimed at the next poll`);
            this.nextPoll = 0;
        }
    }

    private onUpdate(): void {
        if (this.marks.size === 0 && claimOutbox.size === 0) {
            return;
        }
        const now = Date.now();
        if (now < this.nextPoll) {
            return;
        }
        this.nextPoll = now + POLL_MS;
        for (const sceneId of this.marks.sceneIds()) {
            const scene = this.sp.Scene.from(this.sp.Game.getFormEx(sceneId));
            const state = this.marks.observe(sceneId, scene !== null && scene.isPlaying(), now);
            if (state === "playing" || state === "waiting") {
                continue;
            }
            const how = this.marks.how(sceneId);
            const own = this.marks.isOwn(sceneId);
            const claims = this.marks.drop(sceneId);
            for (const c of claims) {
                const message = claimMessage(c.remoteId, false, { purpose: CLAIM_SCENE, quest: c.quest, scene: sceneId });
                claimOutbox.push(message, `scene ${hex(sceneId)} of ${hex(c.quest)} released ${hex(c.remoteId)}: the scene ${state === "ended" ? "ended here" : "never played here"} (claimed ${((now - c.at) / 1000).toFixed(1)} s ago)`);
            }
            const end = state === "ended" ? "ended here" : "never played here within its wait";
            if (own) {
                logLab(TAG, `scene ${hex(sceneId)} (${how}) ${end}: ${claims.length} claim(s) released`);
            } else {
                logTrace(this, `scene ${hex(sceneId)} (${how}) ${end}: the cause seat's mark let go`);
            }
        }
        if (this.marks.size > 0) {
            this.claimActors(now);
        }
        this.flush(now);
    }

    // each high-process actor whose current scene is an own mark playing
    // here, claimed once (PuppetService's list; the player aside, and a
    // server copy with no remote id)
    private claimActors(now: number): void {
        const host = this.controller.lookupListener(HostService);
        for (const localId of this.sp.getHighProcessActors()) {
            if (localId === 0x14) {
                continue;
            }
            const remoteId = localIdToRemoteId(localId);
            if (!remoteId) {
                continue;
            }
            const actor = this.sp.Actor.from(this.sp.Game.getFormEx(localId));
            if (!actor || actor.isDead()) {
                continue;
            }
            const scene = actor.getCurrentScene();
            if (!scene) {
                continue;
            }
            const sceneId = scene.getFormID();
            if (!this.marks.claims(sceneId) || this.marks.claimed(localId, sceneId)) {
                continue;
            }
            const owner = scene.getOwningQuest();
            const quest = owner ? owner.getFormID() : 0;
            if (!isRecordId(quest)) {
                continue;
            }
            const mine = host.isMineLocal(localId);
            this.marks.noteClaim({ localId, remoteId, sceneId, quest, at: now, mine });
            const message = claimMessage(remoteId, true, { purpose: CLAIM_SCENE, quest, scene: sceneId });
            claimOutbox.push(message, `scene ${hex(sceneId)} of ${hex(quest)} claimed ${hex(remoteId)} (local ${hex(localId)}, ${mine ? "this seat's" : "a puppet here"}): this seat started the scene (${this.marks.how(sceneId)}) and its engine plays it on the actor`);
        }
    }

    // the outbox's messages the bucket lets go now, each with its line
    private flush(now: number): void {
        for (const { message, line } of claimOutbox.drain(now)) {
            this.controller.emitter.emit("sendMessageWithRefrId", { message, reliability: "reliable" });
            logLab(TAG, line);
        }
    }

    // a new connection: the server ended this seat's claims at its
    // departure; the marks stay and a scene still playing claims again
    private onConnectionAccepted(): void {
        const n = this.marks.clearClaims();
        claimOutbox.clear();
        this.heldLines.clear();
        if (n > 0) {
            logLab(TAG, `a new connection: ${n} scene claim(s) forgotten (the server ended them at the departure)`);
        }
    }

    private readonly marks = new SceneMarks();
    private readonly heldLines = new Set<string>();
    private nextPoll = 0;
}
