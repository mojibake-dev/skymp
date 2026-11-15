import { logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { OwnHeadPartsMessage } from "../messages/ownHeadPartsMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { ConnectionMessage } from "../events/connectionMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// thuum D-53-8 B (train 53's V-live; lab/.cache/drafts/reads-53/
// stage53-reads.md, train53-greens): after a script's SetRace on this seat's
// own player (ScriptCallService's own row, which the server records on the
// change form), this game's engine may draw the player's face from a list
// the server never sees. Train 53 read it on Ariadne's turn
// (20261115-031800): KhajiitRaceVampire carries the Overlay Head Part List
// flag, her own game copied her parts into the overlay list with
// FemaleEyesKhajiitVampire 0xee87f in her eyes' place and drew that list,
// while every other game's figure of her loaded her stored look's
// FemaleEyesKhajiitBlueNarrow 0xee878. So this game owns its own player's
// look after a race change (ADR-063: the owner computes) and reports the
// head parts it draws as OwnHeadParts 82, R2: the server records them on the
// look and the stored RaceMenu look and sends both to every other game that
// sees the player, as a recorded ChangeHeadPart is. The read is the
// platform's (pollOwnHeadParts on the game thread, ownHeadParts the last
// answer; CommonLibSSE-NG TESNPC: the overlay list while HasOverlays answers
// true and the list holds parts, else the base list).
//
// The window: from the SetRace's note for windowMs, a read every pollEveryMs
// (one game-thread task each, rule 9). A read counts once it came after the
// note (its seq past the one standing then) under the race the script set,
// and no sooner than settleMs after the note (the SetRace row goes out on the
// update after it, through the record bucket, so the server records the race
// first). A race with a head part list flag waits up to overlayWaitMs for
// the engine's overlay list, then takes what the engine draws. Each list that
// differs from the last one sent goes once, so a base list read before the
// overlay came is followed by the overlay list, and the last one sent goes
// once more when the window ends: a report refused because it reached the
// server before the SetRace's record (E_HEADPARTS_RACE, the row still in the
// record bucket) lands then, and one recorded already folds. A race
// overlay's SetRace (the beast form, D-T1 A) reports too, and the server
// refuses it by name (E_HEADPARTS_RACE): the overlay never reaches the
// change form.
//
// thuum D-53-11 (A): the server holds an overlay list's report in memory,
// never on the change form, and drops it at the disconnect, so the login
// opens a window too. Its race is the one the login's look records
// (CreateActor's appearance), and it reports only an overlay list under a
// race with a head part list flag: a base list is the record the login put
// on the base. The login's look goes on at the update after CreateActor and
// RaceMenuService's alignRace may SetRace on its next pass, so the window
// runs loginWindowMs, counts no read sooner than loginSettleMs after the
// login, and starts again while a loading screen is up.
//
// Lines for the lab: "own head parts reported: <n> from its <overlay|base>
// list under race <race> (D-53-8 B)", "own head parts reported after the
// login: <n> from its overlay list under race <race> (D-53-11)", "own head parts sent again at the
// window's end: ..." for the last one, and "own head parts unreported under
// race <race> ..." when a window ends with nothing sent. Off with
// `ownHeadParts: false` in skymp5-client's settings; an older platform
// without the two exports reads nothing (said once).
type Watch = {
    race: number;
    since: number;
    polledAt: number;
    baseSeq: number | undefined;
    sent: string | undefined;
    last: OwnHeadPartsMessage | undefined;
    login: boolean;
};

export class OwnLookService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
    }

    // thuum D-53-11: this game's login (its own CreateActor) opens the login
    // window under the race its look records. JS state alone
    private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
        const race = e.message.isMe && e.message.appearance ? e.message.appearance.raceId : 0;
        if (race === 0 || !this.enabled()) {
            return;
        }
        this.watch = { race, since: Date.now(), polledAt: 0, baseSeq: undefined, sent: undefined, last: undefined, login: true };
    }

    // ScriptCallService's own SetRace on this seat's player, the race its
    // script set by this game's id (a load-order form, the same on the
    // server): the window opens. JS state alone, so the batch's event may
    // call it
    noteOwnRace(race: number): void {
        if (race === 0 || !this.enabled()) {
            return;
        }
        this.watch = { race, since: Date.now(), polledAt: 0, baseSeq: undefined, sent: undefined, last: undefined, login: false };
    }

    private onUpdate() {
        const w = this.watch;
        if (w === undefined) {
            return;
        }
        const now = Date.now();
        if (w.login && this.sp.Ui.isMenuOpen("Loading Menu")) {
            // the login window counts from the world's load
            w.since = now;
            return;
        }
        if (w.baseSeq === undefined) {
            // the read standing at the note: an answer must come after it
            const standing = this.sp.ownHeadParts();
            w.baseSeq = standing ? standing.seq : 0;
        }
        const windowMs = w.login ? OwnLookService.loginWindowMs : OwnLookService.windowMs;
        if (now - w.since > windowMs) {
            if (w.last === undefined && w.login) {
                logTrace(this, `own head parts after the login: no overlay list read under race ${w.race.toString(16)} in ${windowMs} ms, nothing to report (D-53-11)`);
            } else if (w.last === undefined) {
                logLab("verb:appearance", `own head parts unreported under race ${w.race.toString(16)}: no read of it in ${OwnLookService.windowMs} ms (D-53-8 B)`);
            } else {
                // the fresh review of 84eb42db, LOW 9: once more, so a report
                // refused for a SetRace not yet recorded lands, and the
                // server folds it otherwise
                this.controller.emitter.emit("sendMessage", { message: w.last, reliability: "reliable" });
                logLab("verb:appearance", `own head parts sent again at the window's end: ${w.last.parts.length} from its ${w.last.overlay ? "overlay" : "base"} list under race ${w.last.race.toString(16)} (D-53-8 B)`);
            }
            this.watch = undefined;
            return;
        }
        if (now - w.polledAt < OwnLookService.pollEveryMs) {
            return;
        }
        w.polledAt = now;
        const read = this.sp.ownHeadParts();
        this.sp.pollOwnHeadParts();
        const settleMs = w.login ? OwnLookService.loginSettleMs : OwnLookService.settleMs;
        if (!read || read.seq <= w.baseSeq || read.race !== w.race || now - w.since < settleMs) {
            return;
        }
        if (w.login && (!read.flagged || !read.overlay)) {
            return;
        }
        if (read.flagged && !read.overlay && now - w.since < OwnLookService.overlayWaitMs) {
            return;
        }
        if (read.parts.length === 0 || read.parts.length > OwnLookService.maxParts) {
            if (w.sent !== "unsendable") {
                w.sent = "unsendable";
                logLab("verb:appearance", `own head parts unsendable under race ${read.race.toString(16)}: ${read.parts.length} parts, the wire takes 1 to ${OwnLookService.maxParts} (D-53-8 B)`);
            }
            return;
        }
        const key = `${read.overlay ? "overlay" : "base"} ${read.parts.join(",")}`;
        if (key === w.sent) {
            return;
        }
        w.sent = key;
        const message: OwnHeadPartsMessage = {
            t: MsgType.OwnHeadParts,
            race: read.race,
            parts: read.parts,
            overlay: read.overlay,
        };
        w.last = message;
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        if (w.login) {
            logLab("verb:appearance", `own head parts reported after the login: ${read.parts.length} from its overlay list under race ${read.race.toString(16)} (D-53-11)`);
        } else {
            logLab("verb:appearance", `own head parts reported: ${read.parts.length} from its ${read.overlay ? "overlay" : "base"} list under race ${read.race.toString(16)} (D-53-8 B)`);
        }
        // the one read that departs from CommonLib's (the platform's
        // pollOwnHeadParts): HasOverlays true over an empty overlay list,
        // where CommonLib's reading names no part and the base list is taken
        const departs = read.hasOverlays && read.overlayCount === 0 ? "; HasOverlays true over an empty overlay list, so the base list taken where CommonLib's reading names no part" : "";
        logTrace(this, `own head parts ${read.parts.map((p) => p.toString(16)).join(" ")}; HasOverlays ${read.hasOverlays}, ${read.overlayCount} overlay part(s), the race ${read.flagged ? "flagged" : "unflagged"}, ${now - w.since} ms after the SetRace${departs}`);
    }

    private enabled(): boolean {
        if (this.on === undefined) {
            const setting = this.sp.settings["skymp5-client"]["ownHeadParts"] !== false;
            const platform = typeof this.sp.pollOwnHeadParts === "function" && typeof this.sp.ownHeadParts === "function";
            if (setting && !platform) {
                logTrace(this, "pollOwnHeadParts unavailable on this platform: no own head parts report (D-53-8 B)");
            }
            this.on = setting && platform;
        }
        return this.on;
    }

    private watch: Watch | undefined = undefined;
    private on: boolean | undefined = undefined;
    private static readonly windowMs = 20000;
    private static readonly pollEveryMs = 250;
    private static readonly settleMs = 1000;
    private static readonly overlayWaitMs = 5000;
    // D-53-11's login window: the look's apply, alignRace's pass and
    // RaceMenu's preset load all come after the login
    private static readonly loginWindowMs = 60000;
    private static readonly loginSettleMs = 5000;
    // wire-schema cap::HEADPARTS
    private static readonly maxParts = 64;
}
