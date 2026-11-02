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
// login does not permit are the ones to take off the race menu (RaceSexMenu
// lists the races whose RACE record carries the Playable flag, read when the
// menu opens; clearing that flag in memory keeps the plugin's bytes and the
// load order identical on every seat). The hide itself is OFF for now: the
// seats' loaded Race.pex defines MakeUnplayable as a script function, not a
// native one, so callNative throws "Function is not native" (b-race-permit
// 20261101-232833); it returns with a TESModPlatform native declared through
// lab/pex.py add-native (the pex rule) or SKSE's own Race.pex laid by the
// layer. Until then this service checks the lists and logs them, and the
// server's refusal (E_APPEARANCE_RACE) is the whole enforcement. A race the
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
            if (!race.isPlayable()) {
                continue;
            }
            // the hide's native is not on the seats yet (see the header); the
            // server refuses the race whatever the menu lists
            logTrace(this, "race", hex(id), "stays listed; the hide awaits its native");
        }
    }
}

const hex = (id: number): string => id.toString(16);
