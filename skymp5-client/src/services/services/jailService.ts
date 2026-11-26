import { ArrestEvent, CrimeGoldEvent, Faction, ServePrisonTimeEvent } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { ArrestMessage } from "../messages/arrestMessage";
import { CrimeGoldMessage } from "../messages/crimeGoldMessage";
import { JailTermMessage } from "../messages/jailTermMessage";
import { ConnectionMessage } from "../events/connectionMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

const TAG = "verb:jail";
// Skyrim.esm GLOB TimeScale (timeService.ts's timeScaleId)
const kTimeScaleId = 0x3a;

// This game's player's arrest is its own game's, its sentence runs on the
// shared clock, and its bounty per hold is the server's record (R0).
//
// - arrest (the platform's, from PlayerCharacter::GoToPrison's entry, a real
//   jail only, before the engine strips and moves the player; Hooks.cpp
//   PlayerGoToPrison): an Arrest with the faction and the sentence the
//   engine's own rule gives, so the server takes the strip into the
//   faction's two chests and its jail outfit as this game's. It goes at
//   once, ahead of the strip's containerChanged events, which the same
//   update queue raises after it (assumed from the queue's order, not yet
//   traced).
// - crimeGold (the platform's, after the engine changed the player's crime
//   gold for a faction): a CrimeGold with the faction's violent and
//   non-violent gold as Papyrus reads them (Faction.GetCrimeGoldViolent and
//   GetCrimeGoldNonViolent), when they differ from what this game last sent.
//   The server's own tell at a login or a release (Faction.SetCrimeGold and
//   SetCrimeGoldViolent) comes back through here: its two calls raise two
//   events, so the first may send a half-told pair for a moment, which the
//   second corrects to the values the server holds.
// - servePrisonTime (the platform's, the jail bed's serve, which never runs:
//   its time skip would be this game's alone): the sentence is waited out on
//   the shared clock, and the player is told how much of it is left from the
//   term the server holds.
// - jailTermMessage (the server's JailTerm): the term as the server's record
//   holds it, its end on the shared clock, at the arrest's record, at every
//   login while it stands and once when the clock passes the end; kept whole,
//   the newest. The end is the record's, never this game's own clock at the
//   arrest: a game that did not see the arrest (a relaunch) knows it too.
//
// - the release: a served term (the server's JailTerm, kept in `term`) is
//   released by this game's own engine once this connection's login spawn has
//   settled (releaseIfServed); the server gives the prisoner's own belongings
//   back when the move out leaves the jail's cell.
interface JailTerm {
    faction: number;
    releaseAt: number;
    served: boolean;
}

const TERM_KEY = "jailService.term";
const SPAWN_SETTLED_KEY = "jailService.spawnSettled";

export class JailService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        // an older platform without the events refuses the subscription (the
        // summon service's guard); no arrest or bounty is reported then
        try {
            this.controller.on("arrest", (e) => this.onArrest(e));
            this.controller.on("crimeGold", (e) => this.onCrimeGold(e));
            this.controller.on("servePrisonTime", (e) => this.onServePrisonTime(e));
        } catch (e) {
            logTrace(this, `the jail's events are unavailable on this platform (${e}); no arrest or bounty is reported (#97)`);
        }
        this.controller.emitter.on("jailTermMessage", (e) => this.onJailTermMessage(e));
        // a new connection (a reconnect, or another character in this game)
        // is told its own term after its login; the stored one was the
        // earlier connection's, and its spawn settles again
        this.controller.emitter.on("connectionAccepted", () => {
            this.term = undefined;
            this.spawnSettled = false;
        });
        this.controller.emitter.on("ownSpawnSettled", () => { this.spawnSettled = true; });
        this.controller.on("update", () => this.releaseIfServed());
    }

    private onJailTermMessage(e: ConnectionMessage<JailTermMessage>): void {
        const m = e.message;
        this.term = { faction: m.faction, releaseAt: m.releaseAt, served: m.served };
        logLab(TAG, `the server's term by ${m.faction.toString(16)}: ends at day ${m.releaseAt.toFixed(3)} of the shared clock${m.served ? ", served" : ""}`);
    }

    // A served term is released once this connection's own login spawn has
    // settled, so the login's re-sent move to the record's spot, inside the
    // jail, never pulls the released player back. The release is the engine's
    // own fine path with the hold's bounty cleared first: PayFine then takes
    // the bounty's gold (none), and its goToJail has the player's next update
    // run the move out past the jail's door, which clears the sentence and the
    // prison faction (PayFine 40659 and the move out 40661). Once per term. The
    // update's context
    private releaseIfServed(): void {
        const term = this.term;
        if (!term || !term.served || !this.spawnSettled) {
            return;
        }
        // the move out is a far move, whose close of an open Dialogue Menu is
        // a soft hide that can leave the menu on the stack with its input
        // contexts and the player's controls waiting on its close (40438's
        // PositionPlayerEvent, MenuTopicManager's sink 35273, DialogueMenu's
        // ProcessMessage 51506), so it waits for the dialogue to close
        if (this.sp.Ui.isMenuOpen("Dialogue Menu")) {
            return;
        }
        // the term is kept until its release has run: a faction this game
        // does not find, or a fine call that throws, is asked again at the
        // next update (the calls are safe to repeat: two set the bounty to 0,
        // and the fine path's own runs last), its reason said once per term
        const hex = term.faction.toString(16);
        const faction = Faction.from(this.sp.Game.getFormEx(term.faction));
        if (!faction) {
            this.sayOnce(term, `the served term by ${hex}: no such faction in this game yet; kept, asked again each update`);
            return;
        }
        try {
            faction.setCrimeGold(0);
            faction.setCrimeGoldViolent(0);
            faction.playerPayCrimeGold(false, true);
        } catch (e) {
            this.sayOnce(term, `the served term by ${hex}: its release threw (${e}); kept, asked again each update`);
            return;
        }
        this.term = undefined;
        logLab(TAG, `the term by ${hex} served at day ${term.releaseAt.toFixed(3)} of the shared clock: released through the engine's own fine path, its bounty cleared first`);
    }

    // a release's held reason, said once per term and reason
    private sayOnce(term: JailTerm, line: string): void {
        const key = `${term.faction}:${term.releaseAt}:${line}`;
        if (this.said !== key) {
            this.said = key;
            logLab(TAG, line);
        }
    }

    private onArrest(e: ArrestEvent): void {
        const message: ArrestMessage = {
            t: MsgType.Arrest,
            faction: e.factionId,
            days: e.days,
        };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab(TAG, `arrested by ${e.factionId.toString(16)} for ${e.days} day(s): Arrest sent (tracker #97)`);
    }

    private onCrimeGold(e: CrimeGoldEvent): void {
        const faction = Faction.from(e.faction);
        if (!faction) {
            return;
        }
        const violent = Math.max(0, Math.floor(faction.getCrimeGoldViolent()));
        const nonViolent = Math.max(0, Math.floor(faction.getCrimeGoldNonViolent()));
        // the engine's fine (PayFine) carries the gold it took from the
        // player, which the engine removes with no destination container, so
        // the record pays it too (the platform's crimeGold event, paidGold)
        const paidGold = Math.max(0, Math.floor(e.paidGold ?? 0));
        const last = this.sent.get(e.factionId);
        if (paidGold === 0 && last && last.violent === violent && last.nonViolent === nonViolent) {
            return;
        }
        this.sent.set(e.factionId, { violent, nonViolent });
        const message: CrimeGoldMessage = {
            t: MsgType.CrimeGold,
            faction: e.factionId,
            violent,
            nonViolent,
            ...(paidGold > 0 ? { paidGold } : {}),
        };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab(TAG, `bounty in ${e.factionId.toString(16)}: ${violent} violent, ${nonViolent} non-violent${paidGold > 0 ? `, ${paidGold} gold paid` : ""}; CrimeGold sent`);
    }

    private onServePrisonTime(e: ServePrisonTimeEvent): void {
        // the record's end on the shared clock, which sets this game's clock
        const term = this.term;
        const left = term === undefined || term.served ? undefined : term.releaseAt - this.sp.Utility.getCurrentGameTime();
        if (left !== undefined && left > 0) {
            const hours = Math.ceil(left * 24);
            this.sp.Debug.notification(`Your sentence is served in time, not in sleep: about ${hours} hour(s)${this.realTimeOf(left)} left`);
        } else {
            this.sp.Debug.notification("Your sentence is served in time, not in sleep");
        }
        logLab(TAG, `the bed's serve of ${e.days} day(s) for ${e.factionId.toString(16)} asked and not run (ADR-079.2, tracker #97)`);
    }

    // " (about N minutes)" for `days` of game time at this game's TimeScale
    // (the global 0x3a, game seconds per real second, which TimeService sets
    // from the server's SetGameTime, so the server's rate, never a constant);
    // "" when the global reads no rate
    private realTimeOf(days: number): string {
        const timeScale = this.sp.GlobalVariable.from(this.sp.Game.getFormEx(kTimeScaleId))?.getValue() ?? 0;
        if (!(timeScale > 0)) {
            return "";
        }
        const minutes = Math.ceil((days * 24 * 60) / timeScale);
        return minutes >= 120 ? ` (about ${Math.round(minutes / 60)} real hours)` : ` (about ${minutes} real minute(s))`;
    }

    // the last held reason releaseIfServed said (sayOnce)
    private said: string | undefined = undefined;
    // the bounty per faction last sent, so each change goes once
    private sent = new Map<number, { violent: number; nonViolent: number }>();
    // this game's player's jail term as the server's record holds it (the
    // newest JailTerm), whole; none before the first word or once the term's
    // release has run here. Kept in the platform's storage, which a hot reload
    // of the client keeps: the connection stays, so the server's one tell of
    // it per connection does not come again
    get term(): JailTerm | undefined {
        return this.sp.storage[TERM_KEY] as JailTerm | undefined;
    }
    set term(term: JailTerm | undefined) {
        this.sp.storage[TERM_KEY] = term;
    }
    // this connection's own login spawn has put the player at the record's
    // spot (RemoteServer's ownSpawnSettled); in the storage too, since a
    // reload runs no login spawn again
    private get spawnSettled(): boolean {
        return this.sp.storage[SPAWN_SETTLED_KEY] === true;
    }
    private set spawnSettled(settled: boolean) {
        this.sp.storage[SPAWN_SETTLED_KEY] = settled;
    }
}
