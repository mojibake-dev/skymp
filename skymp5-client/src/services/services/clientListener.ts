import * as sp from "skyrimPlatform";
import { EventEmitterType } from "../events/events";

export interface ClientListenerEvents {
    on: typeof sp.on,
    once: typeof sp.once
};

// these are only available in the git version, so we augment the module
// TODO: use typings from the current codebase, not npm module
declare module "skyrimPlatform" {
    function setTextsVisibility(visibility: 'inheritBrowser' | 'off' | 'on'): void;
    function getTextsVisibility(): 'inheritBrowser' | 'off' | 'on';
    function setTextRefr(textId: number, refrFormId: number): void; // pass 0 to detach
    function setTextRefrNode(textId: number, nodeName: string): void;
    function setTextRefrOffset(textId: number, offset: number[]): void;
    function setTextRefrScreenOffset(textId: number, offset: number[]): void;
    function getTextRefr(textId: number): number;
    function getTextRefrNode(textId: number): string;
    function getTextRefrOffset(textId: number): number[];
    function getTextRefrScreenOffset(textId: number): number[];

    // thuum docs/verbs/poison.md: the fork's Skyrim Platform sends it when the
    // player answers Yes in the game's poison box, which sends no equip event
    interface PoisonAppliedEvent {
        weapon: Weapon | null;
        poison: Potion | null;
        count: number;
    }
    function on(eventName: 'poisonApplied', callback: (event: PoisonAppliedEvent) => void): EventHandle;
    function once(eventName: 'poisonApplied', callback: (event: PoisonAppliedEvent) => void): EventHandle;

    // thuum docs/verbs/cast-action.md: the fork's Skyrim Platform reports
    // every BGSAction the player's actor performs through the engine's action
    // entry, with the idle the engine chose, the graph event it sent, the
    // input type and the actor's behavior graph variables as the graph was
    // asked. No game call is allowed in its handler (the event's context).
    interface ActionPerformedEvent {
        // thuum docs/verbs/npc-stream.md (ADR-050): a hosted actor's action
        // comes with actor null and its form id; the player's with 0x14
        actor: Actor | null;
        actorId: number;
        action: number;
        idle: number;
        eventName: string;
        type: number;
        flags: number;
        result: number;
        variables: { booleans: ArrayBuffer; floats: ArrayBuffer; integers: ArrayBuffer };
    }
    function on(eventName: 'actionPerformed', callback: (event: ActionPerformedEvent) => void): EventHandle;
    function once(eventName: 'actionPerformed', callback: (event: ActionPerformedEvent) => void): EventHandle;
    // thuum ADR-034: one of SKSE's action events for the player (type 4 is
    // the voice's fire, 2 a hand's spell fire; slot 0 left, 1 right, 2
    // voice; sourceForm the spell or shout). No game call in its handler.
    interface SkseActionEvent {
        actor: Actor;
        type: number;
        slot: number;
        sourceForm: number;
    }
    function on(eventName: 'skseAction', callback: (event: SkseActionEvent) => void): EventHandle;
    function once(eventName: 'skseAction', callback: (event: SkseActionEvent) => void): EventHandle;
    // thuum docs/verbs/marksman.md: a projectile the engine launched, with
    // the origin and angles it used (every shooter). No game call in its
    // handler.
    interface ProjectileLaunchedEvent {
        shooter: number;
        projectile: number;
        weapon: number;
        ammo: number;
        spell: number;
        castingSource: number;
        originX: number;
        originY: number;
        originZ: number;
        angleX: number;
        angleZ: number;
        power: number;
        useOrigin: boolean;
        // thuum docs/verbs/spell-cast.md: LaunchData's contactNormal (the
        // pick's normal a target-location launch carries; (0, 0, 1) for a
        // launch with no pick)
        contactNormalX: number;
        contactNormalY: number;
        contactNormalZ: number;
    }
    function on(eventName: 'projectileLaunched', callback: (event: ProjectileLaunchedEvent) => void): EventHandle;
    function once(eventName: 'projectileLaunched', callback: (event: ProjectileLaunchedEvent) => void): EventHandle;
    // thuum docs/verbs/spell-cast.md (the rune's glyph on the observer): the
    // engine's decal placement for the player's own target-location launch
    // (the fork's hook on 44186): the projectile's base form, the pick's
    // position, its unit normal and the surface's material id. No game
    // call in its handler.
    interface RuneDecalEvent {
        projectile: number;
        originX: number;
        originY: number;
        originZ: number;
        normalX: number;
        normalY: number;
        normalZ: number;
        materialId: number;
    }
    function on(eventName: 'runeDecal', callback: (event: RuneDecalEvent) => void): EventHandle;
    function once(eventName: 'runeDecal', callback: (event: RuneDecalEvent) => void): EventHandle;
    // thuum docs/verbs/spell-cast.md: a magic effect applied to an actor,
    // with its caster (Skyrim Platform's magicEffectApply; its event type is
    // skyrimPlatform's own)
    function on(eventName: 'magicEffectApply', callback: (event: MagicEffectApplyEvent) => void): EventHandle;
    function once(eventName: 'magicEffectApply', callback: (event: MagicEffectApplyEvent) => void): EventHandle;
    // thuum docs/research/apocalypse/target-location.md: where the player's
    // target-location cast placed its explosion. No game call in its handler.
    interface AreaEffectPlacedEvent {
        spell: number;
        effect: number;
        x: number;
        y: number;
        z: number;
        dual: boolean;
    }
    function on(eventName: 'areaEffectPlaced', callback: (event: AreaEffectPlacedEvent) => void): EventHandle;
    function once(eventName: 'areaEffectPlaced', callback: (event: AreaEffectPlacedEvent) => void): EventHandle;
    // thuum docs/verbs/marksman.md: an arrow's flight as it starts, its final
    // angles (after the player's auto-aim). No game call in its handler.
    interface ProjectileFlightEvent {
        shooter: number;
        projectile: number;
        weapon: number;
        ammo: number;
        spell: number;
        x: number;
        y: number;
        z: number;
        angleX: number;
        angleZ: number;
        power: number;
    }
    function on(eventName: 'projectileFlight', callback: (event: ProjectileFlightEvent) => void): EventHandle;
    function once(eventName: 'projectileFlight', callback: (event: ProjectileFlightEvent) => void): EventHandle;
    // thuum docs/verbs/prop-physics.md (ADR-058): the player's character
    // controller met a prop's Havok body (the fork's hook on the proxy
    // controller's contact callback, Address Library 79131), one event per
    // reference per 250 ms: the reference's form id, the body's collision
    // layer (4 clutter, 10 props) and its motion type (4 Keyframed, the body
    // of a prop this seat does not simulate). No game call in its handler.
    interface PropContactEvent {
        refrId: number;
        layer: number;
        motionType: number;
    }
    function on(eventName: 'propContact', callback: (event: PropContactEvent) => void): EventHandle;
    function once(eventName: 'propContact', callback: (event: PropContactEvent) => void): EventHandle;

    // thuum docs/verbs/leveling.md (M5): the player's UseSkill (the fork's hook on
    // PlayerCharacter's vtable slot 0xF7, Address Library 40488), raised on the next
    // tick once per landed hit, cast or craft: the skill as its actor value
    // (CommonLibSSE-NG include/RE/A/ActorValues.h, 6 One-handed to 23 Enchanting),
    // the action's base amount (the weapon's base damage for a melee hit) and the
    // source form's id (0 for none). Definitions.txt carries the same; this
    // augmentation is what the client's build sees over the published package
    interface SkillUseEvent {
        skill: number;
        points: number;
        source: number;
    }
    function on(eventName: 'skillUse', callback: (event: SkillUseEvent) => void): EventHandle;
    function once(eventName: 'skillUse', callback: (event: SkillUseEvent) => void): EventHandle;
    // thuum docs/DECISIONS.md ADR-068 (rung 2, the one seam): the tabled natives
    // (docs/NATIVES.md's kind column) a script called in this frame on a subject
    // this seat owns, as the platform's hook on the VM's native dispatch
    // (NativeFunctionBase::Call, CommonLibSSE-NG include/RE/Offsets.h:130;
    // ScriptNatives.cpp) read them from the typed frame, one batch a frame at
    // most: cls and fn the native's class and name, kind its row's, self the
    // subject's form id (a static row's first argument; 0 for none), args the
    // script's arguments by the native's declared types (a form as its id; null
    // for None or an argument the hook could not read; a row with an array
    // parameter never reaches the batch), caller and handler the calling script
    // and function (the frame below the native's), latent the native's flag,
    // stackId the Papyrus stack. No game call in its handler.
    // Rung 3 (the script record; docs/research/rung3-script-record-2026-11-06.md
    // 1(a), the seam's changes): kind "touch" is an owned self's state row,
    // batched with no arguments and once a frame per self, for its record's read
    // alone; callerSelf is the calling frame's self (previousFrame's, through the
    // handle policy as self is read), 0 for none, absent on an older platform.
    // D-W2 A (docs/research/fifty-fixes-2026-11-07.md 7.2(a)): kind "look" is
    // a trap family trigger's own call on itself during a firing this seat's
    // presser started (its action rows, SetMotionType, ApplyHavokImpulse, and
    // Sound.Play, whose self is the Sound and whose subject is the source in
    // its first argument), run here and reported from here
    interface ScriptCall {
        cls: string;
        fn: string;
        kind: "action" | "state" | "everyone" | "touch" | "look";
        self: number;
        callerSelf?: number;
        args: (boolean | number | string | { formId: number } | null)[];
        caller: string;
        handler: string;
        latent: boolean;
        stackId: number;
        // train 52 (D-R1 C's leveled gap): on an AddItem of a leveled list on
        // this seat's own player alone, what the engine's roll put in that
        // player's inventory (the call's AddItemFunctor's container events),
        // each base once, its count summed; empty when the roll landed
        // nothing; absent from every other call and from an older platform
        landed?: { formId: number; count: number }[];
    }
    interface ScriptCallsEvent {
        calls: ScriptCall[];
    }
    // the seam's census every 30 s, one row per counted kind of call: "Class.Name
    // (platform)", " (not hosted)", " (local)", " (unrelayable)", " (unresolved)",
    // " (frame mismatch)", " (skipped)" (the scriptNonOwnerSkip lever, off by default),
    // " (effect)" (train 52: a PlaceAtMe on a self this seat does not own, run inside
    // this seat's own player's effect, its result a local helper) and " (helper)" (a
    // call on such a helper, run here and crossing nowhere),
    // the bare "Class.Name" (an owned self's state row, run here) and "Class.Name on
    // <hex id>" or " on others" (an everyone row's selves); a call in scriptCalls is
    // never counted here. No game call in its handler.
    interface ScriptCensusEvent {
        rows: { row: string; count: number }[];
    }
    function on(eventName: 'scriptCalls', callback: (event: ScriptCallsEvent) => void): EventHandle;
    function once(eventName: 'scriptCalls', callback: (event: ScriptCallsEvent) => void): EventHandle;
    function on(eventName: 'scriptCensus', callback: (event: ScriptCensusEvent) => void): EventHandle;
    function once(eventName: 'scriptCensus', callback: (event: ScriptCensusEvent) => void): EventHandle;
    // thuum docs/verbs/npc-stream.md (the voice slice): a spoken line begins
    // on an actor this game hosts; ids alone, no game call in its handler
    interface TopicInfoEvent {
        speakerId: number;
        topicInfo: number;
        topic: number;
    }
    function on(eventName: 'topicInfo', callback: (event: TopicInfoEvent) => void): EventHandle;
    function once(eventName: 'topicInfo', callback: (event: TopicInfoEvent) => void): EventHandle;

    // thuum docs/verbs/npc-puppet.md: the fork's Skyrim Platform switches an
    // NPC to a puppet (combat stopped, keep-offset cleared, AI off, flagged
    // for the two detours) and back (AI on, package evaluated)
    function setPuppet(actorFormId: number, puppet: boolean): boolean;
    function isPuppet(actorFormId: number): boolean;
    function releasePuppet(actorFormId: number): boolean;
    function setPuppetLocomotion(actorFormId: number, speed: number, directionDeg: number, turnRateDeg: number): boolean;
    function getHighProcessActors(): number[];
    // thuum docs/verbs/npc-stream.md (ADR-050): the hosted set the fork's
    // action hook reads (the server's word per actor, HostService), and the
    // mark that the engine's action entry is open on an actor right now
    function setHosted(actorFormId: number, hosted: boolean): boolean;
    function isHosted(actorFormId: number): boolean;
    function isPerformingAction(actorFormId: number): boolean;
    // thuum docs/DECISIONS.md ADR-068 rung 5 (the acting scope;
    // docs/research/rung5-quests-2026-11-09.md 1(c)): the player a relayed
    // quest request runs for on this seat, a native table written with no game
    // call (Hosted.h's idiom, safe from any context). setActingPlayer names it
    // for ttlMs (1 to 60000): stage 0 to 65535 a quest's stage, -1 its start,
    // -2 its stop, -3 a relayed trigger entry (questId then the trigger's form
    // id, which SendTriggerEvent takes); figureLocalId the acting player's
    // figure here, 0 when this seat holds none; serverId its server id; false,
    // nothing stored, for a quest 0, a stage outside -3 to 65535, a serverId 0
    // or 0x14 as the figure. Inside that scope Game.GetPlayer answers the
    // figure. getActingPlayer answers the server id of the scope that
    // requested the pair here, 0 for none. setPlayerGlobals (PQ-3) names the
    // globals each player keeps (PlayerGlobalsService's watch), replaced whole
    // ([] clears): inside another player's acting scope a script's
    // GlobalVariable.SetValue on one is skipped here. The platform layer
    // writes the bodies on its own branch; an older platform has none, and
    // every use is guarded
    function setActingPlayer(questId: number, stage: number, figureLocalId: number, serverId: number, ttlMs: number): boolean;
    function getActingPlayer(questId: number, stage: number): number;
    // ADR-068 rung 5, D-Q4 C (the platform's QuestCause.h): a cause this
    // seat's own act names for the quest's next start (-1), stop (-2) or
    // stage before the engine makes it, for ttlMs, read as a script call's
    // mark is; ConsoleCommandsService marks a quest command the player
    // typed. Since train 52 the platform stores the word the request makes
    // by the quest's run and its Start Game Enabled flag, read there
    // (QuestCause::WordMade: a stage itself, a start of a stopped quest, a
    // stop of a running one, a typed reset's (-3) stop or start), and
    // answers what it did: marked the word stored or null when the request
    // makes none, beside the run and the flag it read. Null, nothing stored,
    // for a quest 0, a stage outside -3 to 65535, a cause other than 'world'
    // or 'seat', a root past 257 bytes or outside [A-Za-z0-9_.], a ttl
    // outside 1 to 60000, or a quest id no quest has; an older platform has
    // none, and its use is guarded
    function markQuestCause(questId: number, stage: number, cause: 'world' | 'seat', root: string, ttlMs: number): { marked: number | null; running: boolean; startsEnabled: boolean } | null;
    function setPlayerGlobals(globalFormIds: number[]): boolean;
    // rung 5, the ownership rule for the per-player quests (the server's
    // player-class quests, QuestStage 57's login `player`): this seat runs its
    // own player's, so the platform's SendEvent gate lets their scripts'
    // events through here; replaced whole at each call, [] clears it
    function setPlayerQuests(questFormIds: number[]): boolean;
    // thuum ADR-068 D-S3 (traps follow the interaction): a reference's trap
    // role from the platform's own reads (null with no form or no script),
    // the trap bodies another seat streams, and the references whose server
    // record is being applied (their Papyrus events blanked meanwhile). The
    // role's trigger (a script on the reference reaching TrapTriggerBase,
    // never a trap body; playtest 48) is absent from an older platform's, as is
    // exempt (D-W2's fallback: one of its classes named in
    // setTriggerPresserExempt, so the presser rule passes its non-owned
    // presses and every seat fires it, D-S3)
    function getTrapRole(formId: number): { family: boolean; playerOnly: boolean; destroyed: boolean; trigger?: boolean; exempt?: boolean } | null;
    function setPropFollowed(formId: number, followed: boolean): boolean;
    function setRecordApplying(formId: number, applying: boolean): boolean;
    // thuum ADR-071 rung 2 (the sample v2; a prop's place is its body's, never
    // its reference's: havok-props.md E21): each loaded reference's Havok body
    // read on the JS thread, pos its 3D root's world translate in game units,
    // rot that rotation as Euler XYZ degrees (x and z in -180 to 180, y in -90
    // to 90), lin game units a second, ang radians a second, motion Havok's
    // type (1, 2, 3 and 6 dynamic, 4 Keyframed, 5 Fixed), awake its island in
    // the world's active list; an id with no reference, 3D, cell world or
    // rigid body on its root is left out. With all false a body is answered
    // while awake and once more at the read its awake bit fell (rule 9), with
    // all true every body found. setBodyVelocity queues the take-over's
    // velocities to the main thread (true when queued) and its task logs
    // "[verb:prop-physics] velocity set on <id>" or "velocity not set on <id>"
    // with why. Definitions.txt carries the same
    interface BodyRead { formId: number; awake: boolean; motion: number; pos: number[]; rot: number[]; lin: number[]; ang: number[] }
    function readBodies(formIds: number[], all: boolean): BodyRead[];
    function setBodyVelocity(formId: number, lin: number[], ang: number[]): boolean;
    // thuum docs/DECISIONS.md D-W1 A and D-C2 A (a trigger volume's entry is
    // its presser's owner's; docs/research/fifty-fixes-2026-11-07.md 1(b)):
    // the gate's presser rule, off by default in the platform so an older
    // client keeps D-S3, turned on by ActivationService from the client
    // setting triggerPresser. An owned presser's OnTriggerEnter or
    // OnTriggerLeave on a family trigger, or on a world trigger in a cell this
    // seat does not host, is raised on a tick task as triggerEntry: the
    // trigger, the presser, enter or leave, the trigger's family. No game call
    // in its handler. The platform layer writes the bodies on its own branch;
    // an older platform has neither, and every use is guarded (said once)
    interface TriggerEntryEvent {
        refrId: number;
        presserId: number;
        enter: boolean;
        family: boolean;
    }
    function on(eventName: 'triggerEntry', callback: (event: TriggerEntryEvent) => void): EventHandle;
    function once(eventName: 'triggerEntry', callback: (event: TriggerEntryEvent) => void): EventHandle;
    function setTriggerPresserRule(enabled: boolean): boolean;
    // D-W2 A (the plan's 7.2(a)): the gate's look switch, off by default in
    // the platform and acting only with the presser rule on, turned on by
    // ActivationService from the client setting triggerLook; and the fallback
    // lever, the script classes whose non-owned presses the presser rule
    // passes (D-S3's per-seat firing for them, chain-matched without case),
    // empty by default, from the client setting triggerPresserExempt
    function setTriggerLook(enabled: boolean): boolean;
    function setTriggerPresserExempt(classes: string[]): boolean;
    // thuum docs/verbs/traps.md H-P1 (the puzzle lever, a NoPlayer FURN;
    // docs/research/fifty-fixes-2026-11-07.md 1(b), the furniture branch): a
    // FURN's own graph event outside a script action, under Override or not,
    // raised on a tick task at the animation hook's leave with the graph's
    // answer (took), never through the hook's handlers; off by default in
    // the platform, turned on by ObjectAnimationService from the client
    // setting furnitureAnimation. No game call in its handler. The platform
    // layer writes the bodies on its own branch; an older platform has
    // neither, and every use is guarded (said once)
    interface FurnitureAnimationEvent {
        refrId: number;
        event: string;
        took: boolean;
    }
    function on(eventName: 'furnitureAnimation', callback: (event: FurnitureAnimationEvent) => void): EventHandle;
    function once(eventName: 'furnitureAnimation', callback: (event: FurnitureAnimationEvent) => void): EventHandle;
    function setFurnitureAnimation(enabled: boolean): boolean;
    // thuum ADR-068 rung 5, D-Q4 C (the platform's QuestCause.h): a quest's
    // start, stop or stage with its cause as the seam read it at the call that
    // made it ('world' or 'seat') and the root frame of that call's stack
    // ("<Script>.<Function>", empty when no script call made it). The published
    // 2.8.0 types name the quest alone; an older platform raises neither
    // field, and QuestStageService reports as before when they are absent.
    // Optional here as in Definitions.txt, so the two merge
    interface QuestStartStopEvent {
        cause?: 'world' | 'seat';
        root?: string;
    }
    interface QuestStageEvent {
        cause?: 'world' | 'seat';
        root?: string;
    }
    // thuum ADR-068 amendment A (rung 3, the script record): a reference whose
    // served record carries a view, so the gate holds its OnLoad and OnCellLoad
    // (and notes which it held) until ApplyScriptState clears the mark; a plain
    // table write, safe from a message handler (the platform's PuppetApi idiom)
    function setScriptRecordPending(formId: number, pending: boolean): boolean;
    // the mark lowered without an apply (a record given up), answering the
    // load events the gate held for the reference as a JSON array of names
    // ("[]" for none), so the client re-sends them; and the scriptRecord switch
    // told to the platform's touch hook (the "touch" rows of scriptCalls)
    function releaseScriptRecordPending(formId: number): string;
    function setScriptRecordTouches(enabled: boolean): boolean;
    // the voice slice: a puppet says the host's line through the engine's
    // own say with the info given; the result -1 pending, 0 none, 1 started,
    // 2 refused, 3 played by the sound descriptor path (no state byte), 4 deferred
    function sayTopicInfo(actorFormId: number, topicInfoFormId: number): boolean;
    function sayTopicInfoResult(actorFormId: number): number;
    // thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the
    // engine): the server's cause applied through the engine's own damage
    // entry to an actor this game simulates, and the cut of this game's own
    // engine damage (npcEngineDamage's mirror)
    function damageActor(actorFormId: number, health: number, attackerFormId: number, killMove: boolean): boolean;
    // slice 5: the gaze and the face read on the game thread per poll ([target, slot, emotionType, emotionValue, exprOverride, exprIndex, exprValue])
    function pollHostedGaze(actorFormIds: number[]): void;
    function hostedGaze(actorFormId: number): number[] | null;
    function pollNodeRotation(refrId: number, nodeNames: string[]): void;
    function nodeRotation(refrId: number): Array<{ node: string; has: boolean; world: number[]; local: number[] }> | null;
    function setEngineDamageCut(on: boolean): void;
    // thuum D-T4 (ADR-028): whether DamageActor's replacement went in, the player's cut of another actor's damage with it
    function isDamageActorReplaced(): boolean;
    // thuum docs/verbs/npc-talk.md (ADR-052): the player's activation of its crosshair pick on the game thread
    function activatePickRef(): boolean;

    // thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the
    // server's roll for a leveled placement, held ahead of the engine's own
    // roll and imposed by two detours behind a runtime switch (default off)
    function setActorFate(refrId: number, baseId: number, worn: number[], inv: number[]): boolean;
    // ADR-050 decision 3 (train 51): a server-made NPC's fate armed by its base before the copy's spawn
    function armActorFate(baseId: number, fateBaseId: number, worn: number[], inv: number[]): boolean;
    function clearActorFate(refrId: number): boolean;
    function setActorFateEnabled(on: boolean): void;
    function rerollActor(refrId: number): boolean;
    function resurrectActor(refrId: number): boolean;
    function redressActor(refrId: number): boolean;
    // thuum docs/verbs/npc-fate.md (playtest 48, the loot strip): a dead body's
    // record-worn armor that it holds unworn, equipped again after an apply
    function dressBody(refrId: number, worn: number[]): boolean;
    function actorFateCount(): number;
}

export type Sp = Omit<typeof sp, "on" | "once">;

export abstract class ClientListener {
    // Don't let TypeScript treat this class as empty
    private _nonEmptyClassMark = '';
}

export type ClientListenerConstructor<T> = {
    new(...args: any[]): T;
    readonly name: string;
}

export type ListenerLookupController = {
    lookupListener<T extends ClientListener>(constructor: ClientListenerConstructor<T>): T;
};

export type EventsController = {
    readonly on: typeof sp.on,
    readonly once: typeof sp.once,
    readonly emitter: EventEmitterType
};

export type CombinedController = EventsController & ListenerLookupController;
