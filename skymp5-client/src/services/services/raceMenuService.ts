import { Actor, MenuCloseEvent, MenuOpenEvent } from "skyrimPlatform";
import * as fs from "fs";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { RaceMenuPresetMessage } from "../messages/raceMenuPresetMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { remoteIdToLocalId } from "../../view/worldViewMisc";
import { parseRaceOverlay } from "../../sync/raceOverlay";
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

// RaceMenu's quest, whose scripts take its mod events, RSM_RequestTintSave
// among them (0x800 in RaceMenu.esp; playtest 50's read); its runtime id
// from the load order, as the hair color form's
const RACEMENU_QUEST = 0x800;

// The storage key of the state a plugin reload must not lose (sp.storage,
// ADR-041's survivor, hostService's shape): the looks the server sent and
// the player's own. A hot-reloaded client (lab client-hot, ADR-054) keeps its
// connection, so no preset message and no CreateActor comes again, and the
// rebuilt views re-spawn every figure with its vanilla appearance alone:
// playtest 37, finding 44 (Eli: "her face morph is wrong", "that worked but
// it shouldnt have to" of the relaunch that brought the sculpt back)
const LOOK_STORAGE_KEY = "thuum.raceMenu.v1";

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
        this.restore();
    }

    // RaceMenu puts its own saved copy of the player's hair color and tints
    // back on as its menu initializes (racemenu.psc OnMenuInitialized:
    // LoadTints, LoadHair). That copy is taken after this service loads the
    // player's look and after each own appearance apply (RSM_RequestTintSave,
    // sendTintSaveIfDue), so it is the look the server holds. No copy is
    // asked for here: the race menu has already reset the tints and hair
    // color to the race's defaults when this event arrives, and a save then
    // made RaceMenu put the defaults back
    // (x-racemenu-done-probe 20261008-224646: the skin tone at the race's
    // default with the menu open, Eli's playtest eleven: hair and skin
    // "back to the nord colors" after a Done with no change).
    private onMenuOpen(e: MenuOpenEvent) {
        if (e.name !== "RaceSex Menu" || !this.available()) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (player) {
            // the menu builds its race, head part and slider lists from the
            // actor's race, so it is the base's before anything is read
            this.alignRace();
            this.hairColorOnRaceMenuForm(player);
            // Nothing is re-applied here. The "almost right" face at the
            // open (Eli, 2026-10-07 21:3x: darker, palette hair, the
            // unsculpted shape) was the race's head and mouth parts
            // carrying no gender flag, so the menu's rebuild took the Nord
            // ones and RaceMenu's sculpt found no host; fixed in the
            // plugin (rotfern.esp 790d3b6c). Loading the recorded look
            // back at the open, tried in c4eebeeb and 482a331f, wiped the
            // tint layers RaceMenu's own save does not carry (the lips and
            // the nose, lab 2026-10-08 00:26), so it is gone.
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
        // The vanilla menu commits its own slider state AFTER this event
        // (Eli, 2026-10-07: the look saved at the event was the preset's,
        // the face on his seat went back to the menu's stale tone and
        // shape), so the save and the send wait a moment for it: the
        // record is RaceMenu's state once the menu is fully closed. The
        // commit writing the Nord head and mouth over a menu-loaded preset
        // (lab, 21:1x) was the race's parts carrying no gender flag, fixed
        // in the plugin; loading a look back over the commit (77400a0d)
        // wiped the tint layers RaceMenu's own save does not carry, so
        // nothing is loaded here.
        this.sp.Utility.wait(0.25).then(() => this.afterMenuClose());
    }

    private afterMenuClose() {
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return;
        }
        const preset = this.save(player, "thuum-self");
        if (!preset || preset === this.lastSent) {
            return;
        }
        this.lastSent = preset;
        this.persist();
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
        this.persist();
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
            this.resetSince = Date.now();
            this.resetFor = id;
        }
        this.persist();
    }

    // The race menu a login finds open in the record (thuum playtest 48,
    // 2026-11-06; numidium's finding, thuum-cf's fix, agreed by numidium).
    // A game that crashes with the menu open leaves isRaceMenuOpen true in
    // the record, since only the appearance its close sends clears it
    // (MpActor::SetRaceMenuOpen), so the next login opens the menu again:
    // c2 crashed in RaceMenu at 20:38:26Z, and the 20:40Z relaunch logged
    // Ariadne in to the race's default face, her appearanceDump intact on
    // the server (d8a9225f98); closing that menu would have recorded the
    // default over it. remoteServer opened the menu 0.3 s after the first
    // update, before these passes had taken RaceMenu's additions off and put
    // the server's look on, which comes after the first movement (2.4 s
    // after that login on the server's clock). The menu puts RaceMenu's own
    // copy of the tints and the hair color back as it opens (onMenuOpen),
    // and that copy is the one a look's load takes (load's
    // RSM_RequestTintSave). HYPOTHESIS until x-racemenu-login-look reads
    // it: no load had taken a copy of hers in that game, so the menu put
    // the race's defaults back, and the passes then ran inside the menu
    // (update fires there: the lab driver's close-menu runs on update with
    // the menu open, a-racemenu) and took the copy from the defaults it
    // showed. The menu stays reachable at a login on purpose (a new
    // character's creation, the lab's open-race-menu), so a recorded
    // character's (its CreateActor carries an appearance) opens here, on the
    // pass after the one that put the server's look on, when RaceMenu's
    // scripts have taken the copy in, or once the player's world has been
    // up sameLookWaitMs with no look on it (a record without one, a load
    // RaceMenu refused); without RaceMenu, at the first pass. A new
    // character (no appearance) still opens on the race's defaults at once,
    // in remoteServer. Every own CreateActor sets or drops it, so a menu
    // waiting from an earlier login never opens over a later one.
    openRaceMenuAfterOwnLook(open: (() => void) | null) {
        this.menuAfterLook = open ? { open, since: 0 } : null;
    }

    // remoteServer's: every race menu the server opens (`mp racemenu`, and
    // the login's above once its look is on) goes through RaceMenu's copy of
    // the player's tints (the fresh review's note 12; playtest 50): while a
    // tint save is due, the open waits for the pass after its request goes,
    // or sameLookWaitMs at most, so the menu puts the player's own tints back
    // as it opens; with none due, or without RaceMenu, it opens now. JS state
    // alone (a message handler's context: available()'s first read is a
    // native call, so the pass decides when RaceMenu is not known yet)
    openRaceMenuWhenTintsSaved(open: () => void) {
        if (this.raceMenu === false || !this.tintSavePending) {
            open();
            return;
        }
        this.menuAfterTint = { open, since: Date.now() };
        logTrace(this, "the race menu's open waits for RaceMenu's copy of the tints");
    }

    // the open the tint save holds, on the pass after its request (or once
    // sameLookWaitMs has gone, said)
    private openRaceMenuIfTintsSaved() {
        const due = this.menuAfterTint;
        if (!due) {
            return;
        }
        if (this.available() && this.tintSavePending && Date.now() - due.since < RaceMenuService.sameLookWaitMs) {
            return;
        }
        this.menuAfterTint = null;
        logTrace(this, this.tintSavePending
            ? `the race menu opens: RaceMenu's quest took no copy of the tints in ${RaceMenuService.sameLookWaitMs} ms`
            : "the race menu opens: RaceMenu's copy of the tints taken");
        due.open();
    }

    // The waiting race menu opens once the passes before this one put the
    // server's look on the player (the reset through, the look applied), or
    // once the player's world has been up sameLookWaitMs without it: the
    // server sends the look at the player's first movement, and the wait
    // starts with the world, since a fresh launch's CreateActor comes
    // before its load (the bound UNMEASURED beyond playtest 48's 2.4 s)
    private openRaceMenuIfLookOn() {
        const due = this.menuAfterLook;
        if (!due) {
            return;
        }
        const me = this.myId();
        let why = "RaceMenu is not here, the appearance alone is the record";
        if (this.available()) {
            // the look on, and RaceMenu's copy of its tints taken (playtest 50)
            if (!this.resetPending && this.applied.has(me) && !this.tintSavePending) {
                why = "the recorded look is on the player";
            } else {
                if (!this.sp.Game.getPlayer() || this.sp.Ui.isMenuOpen("Loading Menu")) {
                    return; // the wait starts with the player's world
                }
                if (due.since === 0) {
                    due.since = Date.now();
                }
                if (Date.now() - due.since < RaceMenuService.sameLookWaitMs) {
                    return;
                }
                why = (!this.resetPending && this.applied.has(me) ? "RaceMenu's quest took no copy of the tints"
                    : this.wanted.has(me) ? "the server's look did not go on the player" : "no RaceMenu look came for the player")
                    + ` in ${RaceMenuService.sameLookWaitMs} ms`;
            }
        }
        this.menuAfterLook = null;
        logTrace(this, `the race menu the record kept open opens: ${why}`);
        due.open();
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
        // the race menu a login found open waits for the look the passes
        // before this one put on (openRaceMenuAfterOwnLook), and for the
        // tint copy a pass before this one asked for; so does any other open
        // the server asked for (openRaceMenuWhenTintsSaved)
        this.openRaceMenuIfLookOn();
        this.openRaceMenuIfTintsSaved();
        if (this.available()) {
            this.sendTintSaveIfDue();
        }
        if (!this.available() || (this.wanted.size === 0 && !this.resetPending)) {
            return;
        }
        const me = this.myId();
        if (this.resetPending && this.ownLook && this.ownLook.refrId === this.resetFor && this.resetFor === me) {
            // the same character logging in again inside one game: the reset
            // is for another character's RaceMenu additions, and on this one
            // it destroys what the game itself set (playtest twenty: after a
            // re-login rotfern's Root, Pelvis and Spine went from 0.6, 0.72
            // and 0.83 to 1.0 under her race's 1.6, two to three times her
            // size in her own game only; dropTransforms re-evaluates each node
            // from NiOverride's stored base, which is not the skeleton's live
            // scale). So its server preset is awaited a while: the look the
            // game already shows needs neither the reset nor a reload
            const wantedMine = this.wanted.get(me);
            if (wantedMine === undefined && Date.now() - this.resetSince < RaceMenuService.sameLookWaitMs) {
                return; // the server's preset comes at the first movement
            }
            if (wantedMine !== undefined && wantedMine === this.ownLook.preset) {
                this.resetPending = false;
                const player = this.sp.Game.getPlayer();
                const base = player ? player.getBaseObject() : null;
                if (player) {
                    this.applied.set(me, player.getFormID().toString(16) + " on base " + (base ? base.getFormID() : 0).toString(16));
                }
                this.persist();
                logTrace(this, "the same look logged in again; neither reset nor reloaded");
            }
        }
        if (this.resetPending && this.resetPlayer()) {
            this.resetPending = false;
        }
        this.wanted.forEach((preset, actor) => {
            if (actor === me && this.resetPending) {
                return; // the reset first, tried again on the next pass
            }
            if (actor !== me && this.wearsOverlayRace(actor)) {
                return; // a figure on its overlay's race takes no look (rung 5, D-T1 A)
            }
            const target = actor === me
                ? this.sp.Game.getPlayer()
                : this.sp.Actor.from(this.sp.Game.getFormEx(remoteIdToLocalId(actor)));
            if (!target) {
                return; // that player's figure is not in this game now
            }
            if (actor === me && !this.raceAligned(target)) {
                return; // the actor's race first (alignRace, from the next pass), then the look
            }
            const base = target.getBaseObject();
            const where = target.getFormID().toString(16) + " on base " + (base ? base.getFormID() : 0).toString(16);
            if (this.applied.get(actor) === where) {
                return;
            }
            if (this.load(target, actor, preset)) {
                this.applied.set(actor, where);
                if (actor === me) {
                    this.ownLook = { refrId: me, preset };
                }
                this.persist();
                logTrace(this, "applied the RaceMenu look of", actor.toString(16), "to", where);
            } else if (this.failed.get(actor) !== where) {
                this.failed.set(actor, where);
                logError(this, "RaceMenu did not load the look of", actor.toString(16), "onto", where);
            }
        });
    }

    // what a look carries of RaceMenu's own layer, for the trace: sculpt
    // hosts with their vertex counts, RaceMenu slider count, head parts
    private lookSummary(preset: string): string {
        try {
            const look = JSON.parse(preset) as Record<string, unknown>;
            const morphs = (look["morphs"] ?? {}) as Record<string, unknown>;
            const sculpt = Array.isArray(morphs["sculpt"]) ? (morphs["sculpt"] as Record<string, unknown>[]) : [];
            const hosts = sculpt.map((h) => `${h["host"]}:${Array.isArray(h["data"]) ? (h["data"] as unknown[]).length : 0}`).join(",");
            const custom = Array.isArray(morphs["custom"]) ? (morphs["custom"] as unknown[]).length : 0;
            const parts = Array.isArray(look["headParts"]) ? (look["headParts"] as Record<string, unknown>[]).map((p) => p["formIdentifier"]).join(",") : "";
            return `sculpt [${hosts || "none"}] custom ${custom} parts [${parts}]`;
        } catch (err) {
            return "not JSON";
        }
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
        const isPlayer = id === this.myId();
        try {
            fs.mkdirSync(PRESETS, { recursive: true });
            fs.writeFileSync(PRESETS + name + ".jslot", isPlayer ? this.withoutTints(preset) : preset, "utf8");
        } catch (err) {
            logError(this, "writing a RaceMenu preset failed", err);
            return false;
        }
        const hairColor = isPlayer ? this.sp.Game.getFormFromFile(PLAYER_HAIR_COLOR, "RaceMenu.esp") : null;
        const loaded = this.sp.callNative("CharGen", "LoadCharacterPresetEx", undefined, actor, name, hairColor, APPLY_ALL) === true;
        if (loaded && isPlayer) {
            // RaceMenu's scripts take in what changed: sendTintSaveIfDue
            this.markTintSaveDue("the player's look loaded");
            // RaceMenu's apply erases the actor's sculpt and slider entries
            // before it writes the look's (PresetInterface ApplyPresetData),
            // so what each apply carried is the record of what the next
            // head rebuild gets (docs/verbs/racemenu-sync.md, 2026-10-07)
            logTrace(this, "the player's look applied:", this.lookSummary(preset));
        }
        return loaded;
    }

    // The player's own look loads without its tint list. RaceMenu's load
    // puts a preset's tints on the player by place in the player's list
    // (skee PresetInterface.cpp ApplyPresetData: tintMasks.GetNthItem of
    // each tint's index), and a look saved before the whole-list apply
    // numbers its layers from the short list: rotfern's lips and nose went
    // on a second time, at 4 and 5 beside their own 6 and 7
    // (x-racemenu-done-probe 20261008-234126). The player's tints are its
    // appearance's (ADR-026 left them there), whole and in the race's
    // order since 5746fa9a; a figure takes none from a look either way.
    private withoutTints(preset: string): string {
        try {
            const look = JSON.parse(preset) as Record<string, unknown>;
            delete look["tintInfo"];
            return JSON.stringify(look, null, 3);
        } catch (err) {
            return preset;
        }
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
    private alignRace(): boolean {
        const player = this.sp.Game.getPlayer();
        if (!player || this.raceAligned(player)) {
            return false;
        }
        const base = this.sp.ActorBase.from(player.getBaseObject());
        const want = base ? base.getRace() : null;
        if (!want) {
            return false;
        }
        player.setRace(want);
        // the player's own look goes on again under the new race: a head part
        // of the look that is not valid for the old race did not land (lab,
        // 2026-10-07: the own face node wore FemaleHeadNord while the look
        // said RotfernChildHead, the figure on the other seat had it right)
        this.applied.delete(this.myId());
        logTrace(this, "the actor's race follows its base's", want.getFormID().toString(16));
        return true;
    }

    // thuum ADR-068 rung 5, D-T1 A (the fresh review's note 6, H-WW1): a
    // figure rebuilt on its race overlay's race (WerewolfBeastRace) would get
    // its owner's human preset loaded with APPLY_ALL, so a player whose model
    // carries an overlay naming a race takes no RaceMenu look on its figure;
    // the overlay's end rebuilds the figure on its own race, where the look
    // goes on again. JS state alone (the world model)
    private wearsOverlayRace(actor: number): boolean {
        const form = this.controller.lookupListener(RemoteServer).getWorldModel().forms
            .find((f) => f !== undefined && f.refrId === actor);
        return !!form && parseRaceOverlay(form.overlay)?.race !== undefined;
    }

    // the player's actor race pointer equals its base's race
    private raceAligned(player: Actor): boolean {
        const base = this.sp.ActorBase.from(player.getBaseObject());
        const want = base ? base.getRace() : null;
        const have = player.getRace();
        return !!want && !!have && have.getFormID() === want.getFormID();
    }

    // remoteServer's, from an update: the player's own appearance went on (a
    // login's, a reconnect's, the server's UpdateAppearance), so RaceMenu's
    // copy of the tints is due again (with RaceMenu here alone)
    ownAppearanceApplied() {
        if (this.available()) {
            this.markTintSaveDue("the player's appearance applied");
        }
    }

    private markTintSaveDue(why: string) {
        this.tintSavePending = true;
        this.tintSaveWhy = why;
        this.persist();
    }

    // thuum playtest 50 (2026-11-09; `mp racemenu` opened Ariadne's menu with
    // her tints and warpaint reset): the RSM_RequestTintSave a login's look
    // load sent went out before RaceMenu.esp's quest 0x800 had started, and
    // SKSE drops a mod event no script is registered for yet (skse64 2.2.6
    // PapyrusEvents.cpp:277-280), so RaceMenu's copy of the tints stayed empty
    // and its menu put the race's defaults back as it opened. The request is
    // due at each own look's load and own appearance apply, and goes from this
    // pass once that quest has run for a whole pass (its scripts registered)
    // and no menu is open (a copy taken inside one is the menu's own state)
    private sendTintSaveIfDue() {
        if (!this.tintSavePending) {
            return;
        }
        const quest = this.sp.Quest.from(this.sp.Game.getFormFromFile(RACEMENU_QUEST, "RaceMenu.esp"));
        if (!quest || !quest.isRunning()) {
            this.tintQuestSeen = false;
            return;
        }
        if (!this.tintQuestSeen) {
            this.tintQuestSeen = true; // running at this pass: the next one sends
            return;
        }
        if (this.sp.Utility.isInMenuMode() || this.sp.Ui.isMenuOpen("RaceSex Menu") || this.sp.Ui.isMenuOpen("Loading Menu")) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return;
        }
        player.sendModEvent("RSM_RequestTintSave", "", 0);
        this.tintSavePending = false;
        this.persist();
        logTrace(this, `RSM_RequestTintSave sent (${this.tintSaveWhy}): RaceMenu's quest runs, so its copy of the tints is the player's look`);
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

    // The state a plugin reload must not lose, as plain arrays in the
    // platform's storage (ADR-041's survivor): the looks the server sent, the
    // player's own look and what the player's own was applied to. The
    // figures' "applied" marks are NOT kept: a rebuilt view re-spawns a figure
    // on a fresh base with the vanilla appearance alone, so every figure's
    // look goes on again at the first pass after a reload (RaceMenu's load
    // of a look an actor already wears changes nothing); the player's own
    // stays as it is (its reload path is the reset's, playtest twenty)
    private persist() {
        const me = this.ownLook ? this.ownLook.refrId : 0;
        const mine = me ? this.applied.get(me) : undefined;
        this.sp.storage[LOOK_STORAGE_KEY] = {
            wanted: Array.from(this.wanted.entries()),
            lastSent: this.lastSent,
            ownLook: this.ownLook,
            appliedMe: me && mine !== undefined ? [me, mine] : null,
            tintSaveDue: this.tintSavePending ? this.tintSaveWhy : null,
        };
    }

    private restore() {
        const raw: unknown = this.sp.storage[LOOK_STORAGE_KEY];
        if (!raw || typeof raw !== "object") {
            return;
        }
        const stored = raw as Partial<Record<"wanted" | "lastSent" | "ownLook" | "appliedMe" | "tintSaveDue", unknown>>;
        const looks = Array.isArray(stored.wanted)
            ? stored.wanted.filter((x): x is [number, string] => Array.isArray(x) && x.length === 2 && typeof x[0] === "number" && typeof x[1] === "string")
            : [];
        for (const [actor, preset] of looks) {
            this.wanted.set(actor, preset);
        }
        if (typeof stored.lastSent === "string") {
            this.lastSent = stored.lastSent;
        }
        const own = stored.ownLook as { refrId?: unknown; preset?: unknown } | null | undefined;
        if (own && typeof own.refrId === "number" && typeof own.preset === "string") {
            this.ownLook = { refrId: own.refrId, preset: own.preset };
        }
        const mine = stored.appliedMe;
        if (Array.isArray(mine) && mine.length === 2 && typeof mine[0] === "number" && typeof mine[1] === "string") {
            this.applied.set(mine[0], mine[1]);
        }
        if (typeof stored.tintSaveDue === "string") {
            this.tintSavePending = true;
            this.tintSaveWhy = stored.tintSaveDue;
        }
        if (this.wanted.size > 0) {
            logTrace(this, `resumed after a reload: ${this.wanted.size} look(s) kept, the figures' go on again at the next pass`);
        }
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
    // when the player's CreateActor came and for which of its server ids; and
    // the look this game last applied to the player, which a re-login of the
    // same character with the same preset leaves as it is
    private resetSince = 0;
    private resetFor = 0;
    private ownLook: { refrId: number; preset: string } | null = null;
    private static readonly sameLookWaitMs = 10000;
    // the race menu a login found open in a recorded character's record,
    // waiting for the player's look (openRaceMenuAfterOwnLook), and when its
    // wait saw the player's world up first
    private menuAfterLook: { open: () => void; since: number } | null = null;
    private raceMenu: boolean | undefined = undefined;
    // playtest 50: RaceMenu's copy of the player's tints is due (why, for the
    // trace), and RaceMenu's quest was running at the last pass that read it
    private tintSavePending = false;
    private tintSaveWhy = "";
    private tintQuestSeen = false;
    // a race menu the server opened, waiting for RaceMenu's copy of the tints
    private menuAfterTint: { open: () => void; since: number } | null = null;
}
