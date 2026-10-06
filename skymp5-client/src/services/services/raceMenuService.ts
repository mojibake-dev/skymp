import { Actor, MenuCloseEvent } from "skyrimPlatform";
import * as fs from "fs";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { RaceMenuPresetMessage } from "../messages/raceMenuPresetMessage";
import { remoteIdToLocalId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";

// RaceMenu's export folder, where TESModPlatform's RaceMenu natives save and
// load a preset by name, relative to the game's folder as RaceMenu's own UI
// writes it
const EXPORTED = "Data/SKSE/Plugins/CharGen/Exported/";

// thuum docs/verbs/racemenu-sync.md: a player's RaceMenu look follows its
// character. When the race menu closes, the player's look is saved through
// RaceMenu (TESModPlatform.SaveRaceMenuPreset) and sent if it changed. The
// server hands back the player's own after a login and every other player's
// with its figure, which RaceMenu loads (LoadRaceMenuPreset). A figure is
// built again on a new base when its appearance changes, so every two seconds
// a look is applied again to an actor whose base it was not applied to yet.
// Nothing happens without RaceMenu (TESModPlatform.RaceMenuPresetVersion 0,
// as on 1.7.104) or with `raceMenuSync: false` in skymp5-client's settings.
export class RaceMenuService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("menuClose", (e) => this.onMenuClose(e));
        this.controller.emitter.on("raceMenuPresetMessage", (e) => this.onPresetMessage(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onMenuClose(e: MenuCloseEvent) {
        if (e.name !== "RaceSex Menu" || !this.available()) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return;
        }
        const preset = this.save(player, "thuum-self");
        if (!preset || preset === this.lastSent) {
            return;
        }
        this.lastSent = preset;
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.RaceMenuPreset, actor: 0, preset },
            reliability: "reliable",
        });
    }

    private onPresetMessage(e: ConnectionMessage<RaceMenuPresetMessage>) {
        const { actor, preset } = e.message;
        if (actor === this.myId()) {
            // the server's record counts as sent: it is not echoed back
            this.lastSent = preset;
        }
        this.wanted.set(actor, preset);
        this.applied.delete(actor);
    }

    private onUpdate() {
        if (this.wanted.size === 0 || Date.now() - this.lastCheck < 2000) {
            return;
        }
        this.lastCheck = Date.now();
        if (!this.available()) {
            return;
        }
        const me = this.myId();
        this.wanted.forEach((preset, actor) => {
            const target = actor === me
                ? this.sp.Game.getPlayer()
                : this.sp.Actor.from(this.sp.Game.getFormEx(remoteIdToLocalId(actor)));
            if (!target) {
                return; // that player's figure is not in this game now
            }
            const base = target.getBaseObject();
            const baseId = base ? base.getFormID() : 0;
            if (this.applied.get(actor) === baseId) {
                return;
            }
            if (this.load(target, actor, preset)) {
                this.applied.set(actor, baseId);
                logTrace(this, "applied the RaceMenu look of", actor.toString(16), "to", target.getFormID().toString(16));
            }
        });
    }

    private save(actor: Actor, name: string): string | undefined {
        if (this.sp.callNative("TESModPlatform", "SaveRaceMenuPreset", undefined, actor, name) !== true) {
            return undefined;
        }
        try {
            return fs.readFileSync(EXPORTED + name + ".jslot", "utf8");
        } catch (err) {
            logError(this, "reading the saved RaceMenu preset failed", err);
            return undefined;
        }
    }

    private load(actor: Actor, id: number, preset: string): boolean {
        const name = "thuum-" + id.toString(16);
        try {
            fs.mkdirSync(EXPORTED, { recursive: true });
            fs.writeFileSync(EXPORTED + name + ".jslot", preset, "utf8");
        } catch (err) {
            logError(this, "writing a RaceMenu preset failed", err);
            return false;
        }
        return this.sp.callNative("TESModPlatform", "LoadRaceMenuPreset", undefined, actor, name) === true;
    }

    private available(): boolean {
        if (this.raceMenu === undefined) {
            const enabled = this.sp.settings["skymp5-client"]["raceMenuSync"] !== false;
            const version = this.sp.callNative("TESModPlatform", "RaceMenuPresetVersion", undefined) as number;
            // asked until RaceMenu answers: skee may hand over its interfaces
            // after the client's first frames
            if (!enabled) {
                this.raceMenu = false;
            } else if (version > 0) {
                this.raceMenu = true;
            }
        }
        return this.raceMenu === true;
    }

    private myId(): number {
        return this.controller.lookupListener(RemoteServer).getMyRemoteRefrId();
    }

    // the player whose look it is (its server id) -> the preset the server sent
    private wanted = new Map<number, string>();
    // the player whose look it is -> the base form id it was last applied to
    private applied = new Map<number, number>();
    private lastSent = "";
    private lastCheck = 0;
    private raceMenu: boolean | undefined = undefined;
}
