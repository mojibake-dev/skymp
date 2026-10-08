import { Actor, MenuCloseEvent, MenuOpenEvent } from "skyrimPlatform";
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
        this.controller.on("menuOpen", (e) => this.onMenuOpen(e));
        this.controller.on("menuClose", (e) => this.onMenuClose(e));
        this.controller.emitter.on("raceMenuPresetMessage", (e) => this.onPresetMessage(e));
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // RaceMenu puts its own saved copy of the player's hair color and tints
    // back on as its menu initializes (racemenu.psc OnMenuInitialized:
    // LoadTints, LoadHair), and after a login that copy is the login save's
    // unless RaceMenu took in the look this service loaded. Asking it to save
    // the current look as the menu opens keeps the menu on that look (lab,
    // 2026-10-07: the hair color changed as the race menu opened).
    private onMenuOpen(e: MenuOpenEvent) {
        if (e.name !== "RaceSex Menu" || !this.available()) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (player) {
            this.hairColorOnRaceMenuForm(player);
            player.sendModEvent("RSM_RequestTintSave", "", 0);
            // the face as the menu opens, so a preset the menu loads shows
            // as a change (see onMenuClose)
            const open = this.save(player, "thuum-open");
            this.openKey = open ? this.sliderKey(open) : undefined;
        }
    }

    // RaceMenu's SaveHair keeps a hair color as the player's own only when
    // it sits on RaceMenu's form 0x801; a color on any other form reads as
    // "not custom", and LoadHair at the menu's initialization then puts the
    // race palette's color on instead (lab, 2026-10-07: the hair went blonde
    // as the menu opened). SkyMP's appearance apply puts the hair color on a
    // form of its own, so the color moves onto 0x801 before RaceMenu saves.
    private hairColorOnRaceMenuForm(player: Actor) {
        const base = this.sp.ActorBase.from(player.getBaseObject());
        const form = this.sp.ColorForm.from(this.sp.Game.getFormFromFile(PLAYER_HAIR_COLOR, "RaceMenu.esp"));
        const current = base ? base.getHairColor() : null;
        if (!base || !form || !current || current.getFormID() === form.getFormID()) {
            return;
        }
        form.setColor(current.getColor());
        base.setHairColor(form);
    }

    private onMenuClose(e: MenuCloseEvent) {
        if (e.name !== "RaceSex Menu" || !this.available()) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return;
        }
        let preset = this.save(player, "thuum-self");
        if (!preset) {
            return;
        }
        // A preset loaded through RaceMenu's own menu arrives through its
        // sliders: the skin tone loses its alpha and lands on another color,
        // and a head part of a type the vanilla menu has no slider for (the
        // ear of Eli's race) is dropped (lab, 2026-10-07: tint 0 88B1C6 at
        // 1.0 and no ear from the menu's load; A9C5D8 at 0.94 with the ear
        // from CharGen's). The player's sculpt and sliders identify the file
        // the menu loaded, and it is applied again the way the sync applies
        // a look, so what is saved and sent is the preset as its author made
        // it. A face shaped by hand matches no file and stays as it is.
        // Only a preset the menu loaded in this session: the face's sliders
        // match a file now and did not as the menu opened. A face that
        // already matched (the preset loaded in an earlier menu) is being
        // edited, and the file, which carries no body morphs, must not come
        // back over the body sliders set in this menu (lab, 2026-10-07: the
        // body sliders reverted as the menu closed).
        const imported = this.importedPreset(preset);
        if (imported && this.sliderKey(preset) !== this.openKey && this.load(player, this.myId(), imported.text)) {
            logTrace(this, "applied the preset", imported.name, "again after RaceMenu's menu loaded it");
            preset = this.save(player, "thuum-self") ?? preset;
        }
        this.openKey = undefined;
        if (preset === this.lastSent) {
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
        if (Date.now() - this.lastCheck < 2000) {
            return;
        }
        this.lastCheck = Date.now();
        // every pass, not only with the reset: at the reset's pass the base
        // may not have the appearance's race yet (SkyMP applies it on the
        // tick after CreateActor), and never behind a loading screen
        if (!this.resetPending && !this.sp.Ui.isMenuOpen("Loading Menu")) {
            this.alignRace();
        }
        if (!this.available() || (this.wanted.size === 0 && !this.resetPending)) {
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

    // The preset file in RaceMenu's folder whose face sliders equal the
    // saved look's: the one the menu just loaded, or undefined. The sync's
    // own files (thuum-*) are not presets. Compared: the game's face morphs
    // and presets (morphs.default) and RaceMenu's own sliders (morphs.custom,
    // as a name to value map), each value to three places; the sculpt is
    // left out, since RaceMenu's save drops a vertex the sculpt did not
    // move and so never writes the file's block back as it was.
    private importedPreset(saved: string): { name: string; text: string } | undefined {
        const key = this.sliderKey(saved);
        if (key === undefined) {
            return undefined;
        }
        let files: string[];
        try {
            files = fs.readdirSync(PRESETS);
        } catch (err) {
            return undefined;
        }
        for (const file of files) {
            if (!file.toLowerCase().endsWith(".jslot") || file.startsWith("thuum-")) {
                continue;
            }
            try {
                const text = fs.readFileSync(PRESETS + file, "utf8");
                if (this.sliderKey(text) === key) {
                    return { name: file.slice(0, -".jslot".length), text };
                }
            } catch (err) {
                // a file that is no preset is not the one
            }
        }
        return undefined;
    }

    // A look's face sliders as one comparable string, or undefined for a
    // look without RaceMenu's own sliders (a face shaped in the vanilla menu
    // matches no preset file by design)
    private sliderKey(text: string): string | undefined {
        let morphs: Record<string, unknown>;
        try {
            morphs = JSON.parse(text)["morphs"] ?? {};
        } catch (err) {
            return undefined;
        }
        const custom = morphs["custom"];
        if (!Array.isArray(custom) || custom.length === 0) {
            return undefined;
        }
        const round = (v: unknown) => typeof v === "number" ? v.toFixed(3) : String(v);
        const def = (morphs["default"] ?? {}) as Record<string, unknown>;
        const list = (v: unknown) => Array.isArray(v) ? v.map(round).join(",") : "";
        const sliders = custom
            .map((c) => `${(c as Record<string, unknown>)["name"]}=${round((c as Record<string, unknown>)["value"])}`)
            .sort()
            .join(";");
        return `${list(def["morphs"])}|${list(def["presets"])}|${sliders}`;
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

    // The player's actor race pointer follows its base's, through the game's
    // own live race change (Actor.SetRace, what the vampire scripts use).
    // SkyMP's appearance apply changes the base's race only, and an actor
    // that loaded as another race keeps it until a reload: RaceMenu then
    // builds its menu's sliders for that race and composes the player's own
    // face under it (probe 20261008-001656: actor an Orc, base rotfern, no
    // rotfern sliders, the face wrong on its own seat and right on the
    // other). Done from the update pass once the player's world is up and
    // the login reset is through, never on the tick after loadGame, where a
    // SetRace froze the game (lab, 2026-10-07). Measured: Actor.SetRace
    // moves the actor's pointer mid-world (probe 20261008-015301), and a
    // fresh loadGame loads it right; a reconnect inside one game is where
    // the two part.
    private alignRace() {
        const player = this.sp.Game.getPlayer();
        const base = player ? this.sp.ActorBase.from(player.getBaseObject()) : null;
        const want = base ? base.getRace() : null;
        const have = player ? player.getRace() : null;
        if (!player || !want || (have && have.getFormID() === want.getFormID())) {
            return;
        }
        player.setRace(want);
        logTrace(this, "the actor's race follows its base's", want.getFormID().toString(16));
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
    // the face's slider key as the race menu opened (sliderKey), for the
    // menu's close
    private openKey: string | undefined = undefined;
    private lastCheck = 0;
    // the player's own CreateActor came, and RaceMenu's additions are still
    // to be taken off it
    private resetPending = false;
    private raceMenu: boolean | undefined = undefined;
}
