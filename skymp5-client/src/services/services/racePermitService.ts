import { MenuOpenEvent } from "skyrimPlatform";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// thuum docs/verbs/character-creation.md, the race permit: the server
// restricts some races to named profiles (its restrictedRaces setting) and
// refuses a race menu result that wears one without a permit
// (E_APPEARANCE_RACE). The rule is the server's; this service is the menu
// side of it. On the player's own CreateActor at login the server names the
// restricted races (props.restrictedRaces) and the ones this profile may wear
// (props.permittedRaces), both global race form ids in the shared load order,
// absent or empty for a profile with no permit. The restricted races the
// login does not permit are taken off the race menu by clearing the Playable
// bit of the RACE record in memory (RaceSexMenu lists the races whose RACE
// DATA flags carry bit 0x1, Playable, read when the menu opens; UESP, "Skyrim
// Mod:Mod File Format/RACE"), so the plugin's bytes and the load order stay
// identical on every seat. The natives are SKSE's Race.IsRaceFlagSet and
// Race.ClearRaceFlag, declared in the Race.pex the seats load loose (SKSE's,
// Data/Scripts/Source/Race.psc in its archive) and typed by Skyrim Platform;
// that script's IsPlayable and MakeUnplayable are wrappers over them, which
// Skyrim Platform refuses as "Function is not native" (b-race-permit
// 20261101-232833, c2), and Form.isPlayable, which the typed binding
// resolved to, reads the form's own flag, not the race's. The server's
// refusal (E_APPEARANCE_RACE) is the enforcement either way. A race the
// player's own base already wears is never hidden: the server keeps a
// recorded race (is_recorded_race), and the menu rebuilds its lists from the
// actor's race, which must stay offered.
export class RacePermitService extends ClientListener {
    private hidden: number[] = [];

    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.on("menuOpen", (e) => this.onMenuOpen(e));
    }

    private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
        const msg = e.message;
        if (!msg.isMe) {
            return;
        }
        const restricted = msg.props?.restrictedRaces ?? [];
        const permitted = msg.props?.permittedRaces ?? [];
        this.hidden = restricted.filter((id) => permitted.indexOf(id) === -1);
        logTrace(this, "restricted races", JSON.stringify(restricted.map(hex)), "permitted here", JSON.stringify(permitted.map(hex)), "hidden", JSON.stringify(this.hidden.map(hex)));
        // natives have no VM context inside a message handler (the review of
        // 2026-11-01: a throw here would stop the own CreateActor from reaching
        // RemoteServer), so the flags are set on the next update
        this.controller.once("update", () => this.apply());
    }

    // the menu builds its race list from the Playable flags as it opens
    // (raceMenuService aligns the actor's race at this same event, before
    // anything is read), so the flags are set again right here
    private onMenuOpen(e: MenuOpenEvent) {
        if (e.name === "RaceSex Menu") {
            this.apply();
        }
    }

    private apply() {
        if (this.hidden.length === 0) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        const base = player ? this.sp.ActorBase.from(player.getBaseObject()) : null;
        const own = base?.getRace()?.getFormID();
        for (const id of this.hidden) {
            if (id === own) {
                logTrace(this, "race", hex(id), "is the player's own; left on the menu");
                continue;
            }
            const race = this.sp.Race.from(this.sp.Game.getFormEx(id));
            if (!race) {
                logError(this, "E_RACE_PERMIT: restricted race", hex(id), "is not a race in this game's load order");
                continue;
            }
            if (!race.isRaceFlagSet(PLAYABLE)) {
                continue;
            }
            race.clearRaceFlag(PLAYABLE);
            logTrace(this, "race", hex(id), "taken off the menu; playable now:", race.isRaceFlagSet(PLAYABLE));
        }
    }
}

// RACE DATA flags bit 0, Playable (SKSE's Race.psc: kRace_Playable)
const PLAYABLE = 0x1;

const hex = (id: number): string => id.toString(16);
