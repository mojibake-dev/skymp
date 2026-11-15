import { MsgType } from "../../messages";
import { ActorClaimMessage } from "../messages/actorClaimMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";

// thuum docs/DECISIONS.md ADR-073 step 5a, the client half (decision 6 with
// D-L2 B, D-A2 C, D-PO2 A and D-53-4): what LeadClaimService and
// SceneClaimService share, with no game call, so a plain node script runs
// it. A claim of an actor names its purpose (ActorClaim since SCHEMA_VERSION
// 47): 1 a quest's lead, 2 a scene. The server checks the records (the
// lead's package QNAM is its quest, the scene's own PNAM is its quest, the
// actor fills one of their aliases) and judges who simulates the actor; the
// seat only says what its engine runs.

export const CLAIM_LEAD = 1;
export const CLAIM_SCENE = 2;

// the server's row holds a quest's leads up to 4 per actor (wire-rules
// cells.rs; a fifth is refused E_CLAIM_FULL), so a seat never asks a fifth
export const LEADS_PER_ACTOR = 4;

// a quest, a package or a scene the validator takes (wire-validate
// check_actor_claim: never 0, and below 0xff000000, a game file's record)
export function isRecordId(id: number): boolean {
    return id > 0 && id < 0xff000000;
}

export type LeadForm = { purpose: typeof CLAIM_LEAD; quest: number; package: number };
export type SceneForm = { purpose: typeof CLAIM_SCENE; quest: number; scene: number };
export type ClaimForm = LeadForm | SceneForm;

// the message for a claim (claim true) or its release (claim false): a lead
// names its quest and package, a scene its quest and scene, the release as
// its claim did (the validator refuses a lead without its package and a scene
// without its scene, either way)
export function claimMessage(remoteId: number, claim: boolean, form: ClaimForm): MessageWithRefrId<ActorClaimMessage> {
    const message: MessageWithRefrId<ActorClaimMessage> = {
        t: MsgType.ActorClaim,
        claim,
        purpose: form.purpose,
        quest: form.quest,
        _refrId: remoteId,
    };
    if (form.purpose === CLAIM_LEAD) {
        message.package = form.package;
    } else {
        message.scene = form.scene;
    }
    return message;
}

// The outbox both services send through, its bucket the server's claim
// budget (wire-validate's claim_budget: 8 at once, then 2 a second,
// UNMEASURED) less a talk's: TalkService sends outside it, so its claims keep
// 2 at once and half a token a second clear of the leads' and the scenes'.
// A message past the bucket waits here instead of being refused there (a
// refused release would leave the server's row standing); nothing is dropped
// (ClaimOutbox.push). AliasFillService's mirrored bucket.
export const CLAIM_BURST = 6;
export const CLAIM_PER_S = 1.5;
export const OUTBOX_MAX = 64;

export type Outgoing = { message: MessageWithRefrId<ActorClaimMessage>; line: string };

export class ClaimOutbox {
    // a release cancels its own claim still queued (the server hears
    // neither); nothing else is dropped: a lost release leaves the server's
    // row standing with no timer to end it (ADR-063), a lost claim leaves the
    // service holding what the server never granted. Each service asks a key
    // once, so the queue is bounded by the actors it claims; past OUTBOX_MAX
    // it is only counted
    push(message: MessageWithRefrId<ActorClaimMessage>, line: string): void {
        if (!message.claim) {
            const queued = this.queue.findIndex((o) => o.message.claim && o.message._refrId === message._refrId && o.message.purpose === message.purpose && o.message.quest === message.quest && o.message.scene === message.scene);
            if (queued >= 0) {
                this.queue.splice(queued, 1);
                return;
            }
        }
        if (this.queue.length >= OUTBOX_MAX) {
            this.overBound++;
        }
        this.queue.push({ message, line });
    }

    // what the bucket lets go now, oldest first
    drain(now: number): Outgoing[] {
        this.tokens = Math.min(CLAIM_BURST, this.tokens + (Math.max(0, now - this.refilledAt) / 1000) * CLAIM_PER_S);
        this.refilledAt = now;
        const out: Outgoing[] = [];
        while (this.queue.length > 0 && this.tokens >= 1) {
            this.tokens -= 1;
            out.push(this.queue.shift() as Outgoing);
        }
        return out;
    }

    // a new connection: the server holds none of this seat's claims, and a
    // release for the last connection's names nothing
    clear(): void {
        this.queue = [];
    }

    get size(): number {
        return this.queue.length;
    }

    overBound = 0;
    private queue: Outgoing[] = [];
    private tokens = CLAIM_BURST;
    private refilledAt = 0;
}

export const claimOutbox = new ClaimOutbox();

// A held lead's reading, each answer asked of the engine only when the one
// before it did not decide (a lazy reading: a lead on an actor the word took
// reads no alias)
export type LeadReading = {
    present: () => boolean;
    dead: () => boolean;
    mine: () => boolean;
    questRunning: () => boolean;
    aliasNames: () => boolean;
};

export type LeadVerdict = { act: "keep" } | { act: "forget"; why: string } | { act: "release"; why: string };

// D-53-4: a quest's own package on that quest's NPC is the quest's claim,
// until the quest releases her, stops, or she dies. The seat forgets a lead
// the server ends itself (her death; the word naming another seat: a
// refusal, a holder's silence) and releases one its engine ends (the actor
// gone from this game, which no server end matches since a lead outlives its
// holder's unload; the quest stopped here; no alias of it naming her here)
export function judgeLead(r: LeadReading): LeadVerdict {
    if (!r.present()) {
        return { act: "release", why: "the actor is not in this game" };
    }
    if (r.dead()) {
        return { act: "forget", why: "her death (the server's death ends the row)" };
    }
    if (!r.mine()) {
        return { act: "forget", why: "the word names another seat" };
    }
    if (!r.questRunning()) {
        return { act: "release", why: "the quest stopped here" };
    }
    if (!r.aliasNames()) {
        return { act: "release", why: "no alias of the quest names her here" };
    }
    return { act: "keep" };
}

// a lead opens on an actor this seat simulates, alive, whose quest runs here
// and one of whose aliases names her, its package already the quest's own
export function opensLead(r: LeadReading): boolean {
    return r.present() && !r.dead() && r.mine() && r.questRunning() && r.aliasNames();
}

// A scene's mark (D-L2 B: the seat that started a scene owns it, claiming its
// actors for its life; D-A2 C: a fragment's scene is the stage's cause
// seat's). An own mark is a scene this seat started: its actors are claimed
// and the puppet rule waits on their claims. A cause seat's mark is this
// seat's copy of a scene a relayed fragment started inside another player's
// acting scope: nothing is claimed for it, and it keeps the scene from being
// taken for this seat's own by its actions here; read after an action that
// marked it own, it disowns that mark (disown) and its claims are released.
// A mark not seen playing within SCENE_START_WAIT_MS is let go (a start the
// engine refused); one seen playing and then not has ended. A claim is
// pending for CLAIM_PENDING_MS (TalkService's unanswered claim, 3 s), from
// its mark until it is pushed to the claims' outbox, then from its push
// (behind a backlog the window can pass before the claim leaves); a claim of
// an actor this seat simulated when it was sent is never pending once she is
// a puppet here (the word took her after the claim: another seat's claim won
// her, the first claim winning).
export const SCENE_START_WAIT_MS = 10_000;
export const CLAIM_PENDING_MS = 3_000;
export const MARKS_MAX = 64;

export type SceneState = "waiting" | "playing" | "ended" | "expired";

// mine: the actor was this seat's to simulate when the claim was sent
export type SceneClaim = { localId: number; remoteId: number; sceneId: number; quest: number; at: number; mine: boolean };

// byAction: own by this seat's sceneAction alone (no start of its own was
// seen), which a relayed fragment's start read after it disowns
type Mark = { since: number; how: string; own: boolean; byAction: boolean; seenPlaying: boolean; playing: boolean };

export class SceneMarks {
    // true for a scene newly marked, or a cause seat's mark this seat's own
    // start makes its own
    mark(sceneId: number, now: number, how: string, own: boolean, byAction = false): boolean {
        const known = this.marks.get(sceneId);
        if (known) {
            if (own && !known.own) {
                known.own = true;
                known.byAction = byAction;
                known.how = how;
                return true;
            }
            return false;
        }
        if (this.marks.size >= MARKS_MAX) {
            const oldest = this.marks.keys().next();
            if (!oldest.done) {
                for (const c of this.drop(oldest.value)) {
                    claimOutbox.push(claimMessage(c.remoteId, false, { purpose: CLAIM_SCENE, quest: c.quest, scene: c.sceneId }), `scene ${c.sceneId.toString(16)} of ${c.quest.toString(16)} released ${c.remoteId.toString(16)}: its mark was evicted past MARKS_MAX`);
                }
            }
        }
        this.marks.set(sceneId, { since: now, how, own, byAction, seenPlaying: false, playing: false });
        return true;
    }

    // a relayed fragment's start read after this seat's own action marked
    // the scene (H-S4's order lost): the cause seat's after all; the
    // claims made under the own mark, to release
    disown(sceneId: number, how: string): SceneClaim[] | null {
        const mark = this.marks.get(sceneId);
        if (!mark || !mark.own || !mark.byAction) {
            return null;
        }
        mark.own = false;
        mark.byAction = false;
        mark.how = how;
        const out: SceneClaim[] = [];
        this.claimsMade.forEach((claim, key) => {
            if (claim.sceneId === sceneId) {
                out.push(claim);
                this.claimsMade.delete(key);
            }
        });
        return out;
    }

    has(sceneId: number): boolean {
        return this.marks.has(sceneId);
    }

    isOwn(sceneId: number): boolean {
        const mark = this.marks.get(sceneId);
        return !!mark && mark.own;
    }

    how(sceneId: number): string {
        const mark = this.marks.get(sceneId);
        return mark ? mark.how : "";
    }

    // the engine's isPlaying for a marked scene, read at a poll
    observe(sceneId: number, playing: boolean, now: number): SceneState {
        const mark = this.marks.get(sceneId);
        if (!mark) {
            return "expired";
        }
        mark.playing = playing;
        if (playing) {
            mark.seenPlaying = true;
            return "playing";
        }
        if (mark.seenPlaying) {
            return "ended";
        }
        return now - mark.since >= SCENE_START_WAIT_MS ? "expired" : "waiting";
    }

    // whether its actors are claimed now: an own mark the last poll read
    // playing
    claims(sceneId: number): boolean {
        const mark = this.marks.get(sceneId);
        return !!mark && mark.own && mark.playing;
    }

    // the mark let go, and the claims made for it (to release)
    drop(sceneId: number): SceneClaim[] {
        this.marks.delete(sceneId);
        const out: SceneClaim[] = [];
        this.claimsMade.forEach((claim, key) => {
            if (claim.sceneId === sceneId) {
                out.push(claim);
                this.claimsMade.delete(key);
            }
        });
        return out;
    }

    claimed(localId: number, sceneId: number): boolean {
        return this.claimsMade.has(SceneMarks.key(localId, sceneId));
    }

    noteClaim(claim: SceneClaim): void {
        this.claimsMade.set(SceneMarks.key(claim.localId, claim.sceneId), claim);
    }

    // PuppetService's question, asked of a puppet: this seat started the
    // scene and its claim of the actor is unsent or unanswered (the owner
    // never stops its own scene while its words are pending)
    holdsStop(localId: number, sceneId: number, now: number): boolean {
        const mark = this.marks.get(sceneId);
        if (!mark || !mark.own) {
            return false;
        }
        const claim = this.claimsMade.get(SceneMarks.key(localId, sceneId));
        if (claim && claim.mine) {
            return false;
        }
        const from = claim ? claim.at : mark.since;
        return now - from < CLAIM_PENDING_MS;
    }

    // a new connection: the server holds none of this seat's claims; the
    // marks stay, so a scene still playing claims its actors again
    clearClaims(): number {
        const n = this.claimsMade.size;
        this.claimsMade.clear();
        return n;
    }

    sceneIds(): number[] {
        return Array.from(this.marks.keys());
    }

    get size(): number {
        return this.marks.size;
    }

    private static key(localId: number, sceneId: number): string {
        return `${localId}:${sceneId}`;
    }

    private readonly marks = new Map<number, Mark>();
    private readonly claimsMade = new Map<string, SceneClaim>();
}
