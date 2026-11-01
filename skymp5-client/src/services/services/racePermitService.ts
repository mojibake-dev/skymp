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
// login does not permit are taken off the race menu with the engine's own
// Race.MakeUnplayable (RaceSexMenu lists the races whose RACE record carries
// the Playable flag, read when the menu opens; MakeUnplayable clears that flag
// in memory, so the plugin's bytes and the load order stay identical on every
// seat). The typed bindings carry Race.isPlayable but not MakeUnplayable, so
// the native is called by name. A race the player's own base already wears is
// never hidden: the server keeps a recorded race (is_recorded_race), and the
// menu rebuilds its lists from the actor's race, which must stay offered.
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
        this.apply();
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
            if (!race.isPlayable()) {
                continue;
            }
            this.sp.callNative("Race", "MakeUnplayable", race);
            logTrace(this, "race", hex(id), "taken off the menu; playable now:", race.isPlayable());
        }
    }
}

const hex = (id: number): string => id.toString(16);
