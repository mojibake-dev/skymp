import { logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { OwnHeadPartsMessage } from "../messages/ownHeadPartsMessage";
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
// overlay came is followed by the overlay list. A race overlay's SetRace (the
// beast form, D-T1 A) reports too, and the server refuses it by name
// (E_HEADPARTS_RACE): the overlay never reaches the change form.
//
// Lines for the lab: "own head parts reported: <n> from its <overlay|base>
// list under race <race> (D-53-8 B)", and "own head parts unreported under
// race <race> ..." when a window ends with nothing sent. Off with
// `ownHeadParts: false` in skymp5-client's settings; an older platform
// without the two exports reads nothing (said once).
type Watch = {
    race: number;
    since: number;
    polledAt: number;
    baseSeq: number | undefined;
    sent: string | undefined;
    sends: number;
};

export class OwnLookService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
    }

    // ScriptCallService's own SetRace on this seat's player, the race its
    // script set by this game's id (a load-order form, the same on the
    // server): the window opens. JS state alone, so the batch's event may
    // call it
    noteOwnRace(race: number): void {
        if (race === 0 || !this.enabled()) {
            return;
        }
        this.watch = { race, since: Date.now(), polledAt: 0, baseSeq: undefined, sent: undefined, sends: 0 };
    }

    private onUpdate() {
        const w = this.watch;
        if (w === undefined) {
            return;
        }
        const now = Date.now();
        if (w.baseSeq === undefined) {
            // the read standing at the note: an answer must come after it
            const standing = this.sp.ownHeadParts();
            w.baseSeq = standing ? standing.seq : 0;
        }
        if (now - w.since > OwnLookService.windowMs) {
            if (w.sends === 0) {
                logLab("verb:appearance", `own head parts unreported under race ${w.race.toString(16)}: no read of it in ${OwnLookService.windowMs} ms (D-53-8 B)`);
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
        if (!read || read.seq <= w.baseSeq || read.race !== w.race || now - w.since < OwnLookService.settleMs) {
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
        w.sends++;
        const message: OwnHeadPartsMessage = {
            t: MsgType.OwnHeadParts,
            race: read.race,
            parts: read.parts,
            overlay: read.overlay,
        };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab("verb:appearance", `own head parts reported: ${read.parts.length} from its ${read.overlay ? "overlay" : "base"} list under race ${read.race.toString(16)} (D-53-8 B)`);
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
    // wire-schema cap::HEADPARTS
    private static readonly maxParts = 64;
}
