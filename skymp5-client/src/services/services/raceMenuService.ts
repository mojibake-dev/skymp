import { Actor, MenuCloseEvent } from "skyrimPlatform";
import * as fs from "fs";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { RaceMenuPresetMessage } from "../messages/raceMenuPresetMessage";
import { remoteIdToLocalId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";

// RaceMenu's preset folder under the game's folder: CharGen's
// SaveCharacterPreset and LoadCharacterPresetEx save and load
// SKSE\Plugins\CharGen\Presets\<name>.jslot in the game's data (RaceMenu's
// scripts\source\chargen.psc)
const PRESETS = "Data/SKSE/Plugins/CharGen/Presets/";

// LoadCharacterPresetEx's flags: every part of a preset, the script's own
// default (0xFFFFFFFF)
const APPLY_ALL = -1;

// RaceMenu's hair color form, which its own LoadPreset hands to the player's
// load (0x801 in RaceMenu.esp)
const PLAYER_HAIR_COLOR = 0x801;

// thuum docs/verbs/racemenu-sync.md: a player's RaceMenu look follows its
// character. When the race menu closes, the player's look is saved through
// RaceMenu's own Papyrus natives (CharGen.SaveCharacterPreset) and sent if it
// changed. The server hands back the player's own after a login and every
// other player's with its figure, which RaceMenu loads
// (CharGen.LoadCharacterPresetEx). A figure is built again on a new base when
// its appearance changes, so every two seconds a look is applied again to an
// actor whose base it was not applied to yet. Nothing happens without
// RaceMenu (no CharGen natives, as on 1.7.104) or with `raceMenuSync: false`
// in skymp5-client's settings.
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

    // RaceMenu's save answers nothing; the file it leaves is the answer
    private save(actor: Actor, name: string): string | undefined {
        const path = PRESETS + name + ".jslot";
        try {
            fs.mkdirSync(PRESETS, { recursive: true });
            fs.rmSync(path, { force: true });
            this.sp.callNative("CharGen", "SaveCharacterPreset", undefined, actor, name);
            return fs.readFileSync(path, "utf8");
        } catch (err) {
            logError(this, "saving the RaceMenu preset failed", err);
            return undefined;
        }
    }

    // The player's own look loads as RaceMenu's LoadPreset loads it: with
    // RaceMenu's hair color form, then RSM_RequestTintSave so RaceMenu's
    // scripts take in what changed. A figure's hair color is its appearance's.
    private load(actor: Actor, id: number, preset: string): boolean {
        const name = "thuum-" + id.toString(16);
        try {
            fs.mkdirSync(PRESETS, { recursive: true });
            fs.writeFileSync(PRESETS + name + ".jslot", preset, "utf8");
        } catch (err) {
            logError(this, "writing a RaceMenu preset failed", err);
            return false;
        }
        const isPlayer = id === this.myId();
        const hairColor = isPlayer ? this.sp.Game.getFormFromFile(PLAYER_HAIR_COLOR, "RaceMenu.esp") : null;
        const loaded = this.sp.callNative("CharGen", "LoadCharacterPresetEx", undefined, actor, name, hairColor, APPLY_ALL) === true;
        if (loaded && isPlayer) {
            actor.sendModEvent("RSM_RequestTintSave", "", 0);
        }
        return loaded;
    }

    private available(): boolean {
        if (this.raceMenu === undefined) {
            const enabled = this.sp.settings["skymp5-client"]["raceMenuSync"] !== false;
            this.raceMenu = enabled && this.hasCharGen();
        }
        return this.raceMenu;
    }

    // RaceMenu's natives are bound before the game reaches its main menu, so
    // one answer holds for the session
    private hasCharGen(): boolean {
        try {
            this.sp.callNative("CharGen", "IsExternalEnabled", undefined);
            return true;
        } catch (err) {
            logTrace(this, "no RaceMenu: CharGen's natives are not there", err);
            return false;
        }
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
