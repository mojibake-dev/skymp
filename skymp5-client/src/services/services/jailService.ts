import { ArrestEvent, CrimeGoldEvent, Faction, ServePrisonTimeEvent } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { ArrestMessage } from "../messages/arrestMessage";
import { CrimeGoldMessage } from "../messages/crimeGoldMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

const TAG = "verb:jail";

// thuum tracker #97, ADR-079 (Eli, 2026-11-20: "yes real jail time";
// docs/verbs/jail.md): this game's player's arrest is its own game's
// (ADR-063), its sentence runs on the shared clock (ADR-021), and its bounty
// per hold is the server's record (D-G6 A, R0).
//
// - arrest (the platform's, from PlayerCharacter::GoToPrison's entry, a real
//   jail only, before the engine strips and moves the player; Hooks.cpp
//   PlayerGoToPrison): Arrest 84 with the faction and the sentence the
//   engine's own rule gives, so the server takes the jump into the faction's
//   jail, the strip into its two chests and its jail outfit as this game's.
//   It goes at once, ahead of the strip's containerChanged events, which the
//   same update queue raises after it (HYPOTHESIS until a trace).
// - crimeGold (the platform's, after the engine changed the player's crime
//   gold for a faction): CrimeGold 85 with the faction's violent and
//   non-violent gold as Papyrus reads them (Faction.GetCrimeGoldViolent and
//   GetCrimeGoldNonViolent), when they differ from what this game last sent.
//   The server's own tell at a login or a release (Faction.SetCrimeGold and
//   SetCrimeGoldViolent) comes back through here: its two calls raise two
//   events, so the first may send a half-told pair for a moment, which the
//   second corrects to the values the server holds.
// - servePrisonTime (the platform's, the jail bed's serve, which never runs:
//   ADR-079.2): the sentence is waited out on the shared clock, and the
//   player is told how much of it is left as this game knows it.
//
// The release is the server's (PartOne::ReleaseIfServed: the prisoner's own
// belongings back, the bounty cleared, the player placed at the jail marker,
// the engine's prison state cleared through Game.ClearPrison), so nothing
// here ends a sentence.
export class JailService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        // an older platform without the events refuses the subscription (the
        // summon service's guard); no arrest or bounty is reported then, the
        // pre-#97 behavior
        try {
            this.controller.on("arrest", (e) => this.onArrest(e));
            this.controller.on("crimeGold", (e) => this.onCrimeGold(e));
            this.controller.on("servePrisonTime", (e) => this.onServePrisonTime(e));
        } catch (e) {
            logTrace(this, `the jail's events are unavailable on this platform (${e}); no arrest or bounty is reported (#97)`);
        }
    }

    private onArrest(e: ArrestEvent): void {
        const message: ArrestMessage = {
            t: MsgType.Arrest,
            faction: e.factionId,
            days: e.days,
        };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        // the sentence's end as this game's clock reads it, for the bed's
        // answer (the server's own end is the record's)
        this.releaseAt = this.sp.Utility.getCurrentGameTime() + e.days;
        logLab(TAG, `arrested by ${e.factionId.toString(16)} for ${e.days} day(s): Arrest sent (tracker #97)`);
    }

    private onCrimeGold(e: CrimeGoldEvent): void {
        const faction = Faction.from(e.faction);
        if (!faction) {
            return;
        }
        const violent = Math.max(0, Math.floor(faction.getCrimeGoldViolent()));
        const nonViolent = Math.max(0, Math.floor(faction.getCrimeGoldNonViolent()));
        const last = this.sent.get(e.factionId);
        if (last && last.violent === violent && last.nonViolent === nonViolent) {
            return;
        }
        this.sent.set(e.factionId, { violent, nonViolent });
        const message: CrimeGoldMessage = {
            t: MsgType.CrimeGold,
            faction: e.factionId,
            violent,
            nonViolent,
        };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab(TAG, `bounty in ${e.factionId.toString(16)}: ${violent} violent, ${nonViolent} non-violent; CrimeGold sent (tracker #97)`);
    }

    private onServePrisonTime(e: ServePrisonTimeEvent): void {
        const left = this.releaseAt === undefined ? undefined : this.releaseAt - this.sp.Utility.getCurrentGameTime();
        if (left !== undefined && left > 0) {
            const hours = Math.ceil(left * 24);
            this.sp.Debug.notification(`Your sentence is served in time, not in sleep: about ${hours} hour(s) left`);
        } else {
            this.sp.Debug.notification("Your sentence is served in time, not in sleep");
        }
        logLab(TAG, `the bed's serve of ${e.days} day(s) for ${e.factionId.toString(16)} asked and not run (ADR-079.2, tracker #97)`);
    }

    // the bounty per faction last sent, so each change goes once
    private sent = new Map<number, { violent: number; nonViolent: number }>();
    // the sentence's end on this game's clock (game days), from its arrest
    private releaseAt: number | undefined = undefined;
}
