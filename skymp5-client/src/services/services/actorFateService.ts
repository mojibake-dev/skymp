import { logLab, logTrace } from "../../logging";
import { ActorFateMessage } from "../messages/actorFateMessage";
import { ConnectionMessage } from "../events/connectionMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the server's
// roll is the one roll. ActorFate names, for every game-file actor in the
// squares this game is about to attach, the NPC_ the server's leveled roll
// chose and the worn list its outfit roll chose; the fates go into the fork's
// Skyrim Platform table (setActorFate), where two native detours make the
// engine roll them: the leveled roll wrapper under the resolver (the
// reference's load and the attach-time re-roll alike) and the outfit
// resolver under every inventory init (re-npc-roll.md section 8). A fate
// for a reference the engine already holds (a login's neighbours, loaded by
// the save before the connect; an interior's actors, loaded at the door
// before the fate arrives; a respawn's new fate) takes the late path: the
// engine's own re-roll (rerollActor) when the engine cloned a leveled pick
// (its base is a runtime form), else the redress (redressActor: the outfit
// stripped, the server's list added tagged, worn again); one pop, accepted
// by ADR-049. The switch npcFate (settings skymp5-client.npcFate) arms the
// detours; on unless the file says false since ADR-046 decision 1 held at
// T3 (x-npc-fate-pin v3 GREEN 20261024-053914 on 0a10f977: both seats show
// the server's species, each pinned at the roll); off, the fates are kept
// and the detours pass every roll through (the bisect switch).
export class ActorFateService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        // the late fate's wait for a load (playtest 41's crash): the attach of
        // the reference's cell and a slow poll
        this.controller.on("cellAttach", (e) => this.onCellAttach(e));
        this.controller.on("update", () => this.onUpdate());
        const setting = this.sp.settings["skymp5-client"]["npcFate"];
        this.enabled = setting !== false;
        this.sp.setActorFateEnabled(this.enabled);
        logTrace(this, this.enabled
            ? "npcFate is on (the default): the engine rolls what the server rolled (the two detours armed)"
            : "npcFate is off by the settings file: fates are kept, the detours pass every roll through");
        this.controller.emitter.on("actorFateMessage", (e) => this.onActorFate(e));
    }

    private onActorFate(event: ConnectionMessage<ActorFateMessage>) {
        // the table write is the handler's own work and stays here: Fates is a
        // mutex-guarded table the detours read on the engine's threads, and the
        // fate has to sit in it before the attach it runs ahead of (a loading
        // screen attaches the cell before the next update tick fires)
        const late: number[] = [];
        for (const fate of event.message.fates) {
            if (fate.refrId >= 0xff000000) {
                // thuum ADR-050 decision 3 (train 51): a server-made NPC's
                // fate, sent ahead of its create word; kept by its remote id
                // until the view arms it for the copy's spawn (armActorFate),
                // where the detours bind it to the copy's own id. No native
                // table entry under the remote id and no late path: the copy
                // is this game's to place
                this.copyFates.set(fate.refrId, { baseId: fate.baseId, worn: fate.worn.slice() });
                continue;
            }
            this.sp.setActorFate(fate.refrId, fate.baseId, fate.worn);
            const key = `${fate.baseId}:${fate.worn.join(",")}`;
            const before = this.applied.get(fate.refrId);
            this.applied.set(fate.refrId, key);
            if (this.enabled && before !== key) {
                late.push(fate.refrId);
            }
        }
        if (late.length === 0) {
            return;
        }
        // the late path asks the engine (Game.getFormEx, Actor.from, the base),
        // so it runs inside the update tick, never in the message handler's
        // context: a native there broke every teleport on train-40 (acaf4b34,
        // 2026-10-23); the two exports queue their own game-thread tasks
        this.controller.once("update", () => {
            for (const refrId of late) {
                this.lateFate(refrId);
            }
        });
    }

    // thuum playtest 41 (2026-11-03 01:51:00Z): c1 crashed in the redress of
    // Sven right after the host set emptied, EXCEPTION_ACCESS_VIOLATION at
    // SkyrimSE.exe+070DB20 on a null read, the platform's last lines two
    // redresses "3D loaded false" and "rerollActor(13491): not a leveled
    // clone, redress instead". HYPOTHESIS: the redress's AddWornOutfit on an
    // actor with no 3D reads the model it has not got. So the late fate
    // runs on a LOADED actor alone: an unloaded one waits in `pending` and
    // is dressed at its load (the cellAttach of its reference, and a poll
    // every PENDING_POLL_MS for a load no attach names), rule 3's shape (a
    // reloading engine renders at load). A reference the engine no longer
    // has is forgotten.
    private lateFate(refrId: number) {
        // the engine has the reference already (the save around the player
        // at a login, a cell attached ahead of the fate, a respawn's new
        // roll): its own re-roll goes through the detour
        const refr = this.sp.Game.getFormEx(refrId);
        const actor = refr ? this.sp.Actor.from(refr) : null;
        if (!actor) {
            this.pending.delete(refrId);
            return;
        }
        if (!actor.is3DLoaded()) {
            if (!this.pending.has(refrId)) {
                this.pending.set(refrId, Date.now());
                logLab("verb:npc-fate", `late fate for ${refrId.toString(16)}: its 3D is not loaded, the redress waits for the load`);
            }
            return;
        }
        this.pending.delete(refrId);
        // the export decides on the game thread by the engine's own mark
        // (ExtraLeveledCreature): a clone is re-rolled through the detours, a
        // plain base is redressed. The Papyrus getBaseObject read before
        // answered a clone's placed base, so no re-roll ever ran
        // (b-npc-respawn 120743)
        const queued = this.sp.rerollActor(refrId);
        logLab("verb:npc-fate", `late fate for ${refrId.toString(16)}: engine re-roll or redress ${queued ? "queued" : "refused"}`);
    }

    private onCellAttach(e: { refr?: { getFormID(): number } | null }) {
        if (!e.refr) {
            return;
        }
        const id = e.refr.getFormID();
        if (this.pending.has(id)) {
            this.lateFate(id);
        }
    }

    private onUpdate() {
        if (this.pending.size === 0) {
            return;
        }
        const now = Date.now();
        if (now - this.lastPendingPoll < ActorFateService.PENDING_POLL_MS) {
            return;
        }
        this.lastPendingPoll = now;
        for (const [refrId, since] of Array.from(this.pending.entries())) {
            if (now - since > ActorFateService.PENDING_MAX_MS) {
                this.pending.delete(refrId);
                logTrace(this, `late fate for ${refrId.toString(16)}: no load within ${ActorFateService.PENDING_MAX_MS / 1000} s, forgotten (the next fate asks again)`);
                continue;
            }
            this.lateFate(refrId);
        }
    }

    // the fates waiting for their actor's 3D, with the time they began to wait
    private readonly pending = new Map<number, number>();
    private lastPendingPoll = 0;
    private static readonly PENDING_POLL_MS = 1000;
    private static readonly PENDING_MAX_MS = 120_000;

    // The server's fate for a server-made NPC by its remote id (ADR-050
    // decision 3), read by the view at the copy's spawn.
    fateForCopy(remoteId: number): { baseId: number; worn: number[] } | undefined {
        return this.copyFates.get(remoteId);
    }

    // the last fate applied per reference (base and worn), so a repeated fate
    // for the same roll re-rolls nothing
    private applied = new Map<number, string>();
    private copyFates = new Map<number, { baseId: number; worn: number[] }>();
    private enabled: boolean;
}
