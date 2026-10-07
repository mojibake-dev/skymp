import { Actor, MenuCloseEvent } from "skyrimPlatform";
import * as fs from "fs";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { RaceMenuPresetMessage } from "../messages/raceMenuPresetMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
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

// The parts of a preset only RaceMenu gives a look, which a reset leaves out:
// node overrides, skin overrides, node transforms and body morphs (top-level
// sections of RaceMenu's preset file), and the sculpt and RaceMenu's own
// sliders (morphs.sculpt, morphs.custom)
const RACEMENU_ONLY = ["overrides", "skinOverrides", "transforms", "bodyMorphs"];

// thuum docs/verbs/racemenu-sync.md: a player's RaceMenu look follows its
// character. When the race menu closes, the player's look is saved through
// RaceMenu's own Papyrus natives (CharGen.SaveCharacterPreset) and sent if it
// changed. The server hands back the player's own after a login and every
// other player's with its figure, which RaceMenu loads
// (CharGen.LoadCharacterPresetEx). RaceMenu writes a look to the actor's base
// (head parts, morphs, tints, the sculpt) and to the reference itself
// (overrides, transforms, body morphs), and a figure is built again, on a new
// base, when its appearance changes; so every two seconds a look is applied
// again to a reference or base it was not applied to yet. Nothing happens
// without RaceMenu (no CharGen natives, as on 1.7.104) or with
// `raceMenuSync: false` in skymp5-client's settings.
export class RaceMenuService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("menuClose", (e) => this.onMenuClose(e));
        this.controller.emitter.on("raceMenuPresetMessage", (e) => this.onPresetMessage(e));
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
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
        this.failed.delete(actor);
    }

    // A CreateActor starts that actor afresh: the server sends its current
    // look right after (with a figure, or once the player's own world is up
    // after a login), so what the service knew of it is dropped. The
    // player's own game may still hold RaceMenu's additions from before, a
    // reconnect inside one game showing a look the server never had (lab,
    // 2026-10-07: an orc wearing the last session's sculpt), so those are
    // taken off before anything of the server's is applied to the player.
    private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
        const id = e.message.refrId;
        if (typeof id !== "number") {
            return;
        }
        this.wanted.delete(id);
        this.applied.delete(id);
        this.failed.delete(id);
        if (e.message.isMe) {
            // nothing the server has not sent since counts as sent
            this.lastSent = "";
            this.resetPending = true;
        }
    }

    private onUpdate() {
        if ((this.wanted.size === 0 && !this.resetPending) || Date.now() - this.lastCheck < 2000) {
            return;
        }
        this.lastCheck = Date.now();
        if (!this.available()) {
            return;
        }
        if (this.resetPending && this.resetPlayer()) {
            this.resetPending = false;
        }
        const me = this.myId();
        this.wanted.forEach((preset, actor) => {
            if (actor === me && this.resetPending) {
                return; // the reset first, tried again on the next pass
            }
            const target = actor === me
                ? this.sp.Game.getPlayer()
                : this.sp.Actor.from(this.sp.Game.getFormEx(remoteIdToLocalId(actor)));
            if (!target) {
                return; // that player's figure is not in this game now
            }
            const base = target.getBaseObject();
            const where = target.getFormID().toString(16) + " on base " + (base ? base.getFormID() : 0).toString(16);
            if (this.applied.get(actor) === where) {
                return;
            }
            if (this.load(target, actor, preset)) {
                this.applied.set(actor, where);
                logTrace(this, "applied the RaceMenu look of", actor.toString(16), "to", where);
            } else if (this.failed.get(actor) !== where) {
                this.failed.set(actor, where);
                logError(this, "RaceMenu did not load the look of", actor.toString(16), "onto", where);
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

    // RaceMenu's load erases an actor's sculpt, its own sliders, overrides
    // and transforms before it applies a preset's (PresetInterface
    // ApplyPresetData), so the player's look saved and loaded back without
    // those parts drops what RaceMenu added and keeps the face SkyMP gave it
    private resetPlayer(): boolean {
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return false;
        }
        this.dropTransforms(player);
        const saved = this.save(player, "thuum-reset");
        if (!saved) {
            return false;
        }
        let look: Record<string, unknown>;
        try {
            look = JSON.parse(saved);
        } catch (err) {
            logError(this, "RaceMenu saved a look that is not JSON", err);
            return false;
        }
        RACEMENU_ONLY.forEach((part) => delete look[part]);
        const morphs = look["morphs"];
        if (morphs && typeof morphs === "object") {
            (morphs as Record<string, unknown>)["sculpt"] = null;
            (morphs as Record<string, unknown>)["custom"] = null;
        }
        const reset = this.load(player, this.myId(), JSON.stringify(look));
        if (reset) {
            logTrace(this, "took RaceMenu's additions off the player before its record");
        }
        return reset;
    }

    // RaceMenu's load erases an actor's transforms from its records but
    // leaves the skeleton as drawn (NiTransformInterface
    // Impl_RemoveAllReferenceTransforms erases the actor's entry, and
    // SetTransforms recomputes only the nodes an entry still lists; lab,
    // 2026-10-07: a head kept its 1.3 after the reset). So each key but
    // RaceMenu's "internal" comes off node by node first and the node is
    // updated from its base, through NiOverride's own calls (0.4.20.0
    // nioverride.psc: GetNodeTransformNames, GetNodeTransformKeys, the
    // RemoveNodeTransform* four, UpdateNodeTransform)
    private dropTransforms(player: Actor) {
        const base = this.sp.ActorBase.from(player.getBaseObject());
        const female = base !== null && base.getSex() === 1;
        const removals = ["RemoveNodeTransformPosition", "RemoveNodeTransformScale",
            "RemoveNodeTransformScaleMode", "RemoveNodeTransformRotation"];
        [false, true].forEach((firstPerson) => {
            const nodes = this.sp.callNative("NiOverride", "GetNodeTransformNames", undefined,
                player, firstPerson, female) as string[] | null;
            (nodes || []).forEach((node) => {
                const keys = this.sp.callNative("NiOverride", "GetNodeTransformKeys", undefined,
                    player, firstPerson, female, node) as string[] | null;
                (keys || []).filter((key) => key !== "internal").forEach((key) => {
                    removals.forEach((fn) => this.sp.callNative("NiOverride", fn, undefined,
                        player, firstPerson, female, node, key));
                });
                this.sp.callNative("NiOverride", "UpdateNodeTransform", undefined, player, firstPerson, female, node);
            });
        });
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
    // the player whose look it is -> the reference and base it was last
    // applied to
    private applied = new Map<number, string>();
    // the player whose look it is -> the reference and base a load last
    // failed on, so each failure is logged once
    private failed = new Map<number, string>();
    private lastSent = "";
    private lastCheck = 0;
    // the player's own CreateActor came, and RaceMenu's additions are still
    // to be taken off it
    private resetPending = false;
    private raceMenu: boolean | undefined = undefined;
}
