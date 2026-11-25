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
    // of a prop this seat does not simulate). Since ADR-071 rung 4, while
    // setWorldBodyContacts(true) holds, also a body of a dynamic motion type
    // (1, 2, 3 or 6) on any other layer (a world body's part, 29 clutter
    // large, 32 a corpse's dead biped); never the player's own reference. No
    // game call in its handler.
    interface PropContactEvent {
        refrId: number;
        layer: number;
        motionType: number;
    }
    function on(eventName: 'propContact', callback: (event: PropContactEvent) => void): EventHandle;
    function once(eventName: 'propContact', callback: (event: PropContactEvent) => void): EventHandle;
    // thuum ADR-071 decision 4 (physics rung 3, "caster only"): the bodies an
    // impulse this seat's own cause gave pushed inside one engine frame (the
    // fork's replacements of ScaleGameplayImpulseForce and AddExplosionImpulse,
    // Address Library 26004 and 26005, judged by the innermost Explosion::Update,
    // Projectile::ProcessImpacts or ConeProjectile::HandleHits on the thread),
    // raised on the next tick while setImpulseCauseRule(true) holds: impact this
    // seat's impact number from 1 (a cone raises a new impact number at each
    // HandleHits frame that pushed, so its events fold by cause for
    // CONE_BATCH_MS, propService.ts; a set past 16 references splits under one
    // number), kind the frame, cause the cause's form id
    // here (0x14, an actor this seat simulates; 0 for none, in a cell this
    // seat hosts), refrs each reference pushed once in push order, every
    // physics body (D-PH5): refrId this seat's form id, formType its base's
    // FormType number, so the client picks its rows. An impulse another seat
    // caused is zeroed in native code and raises nothing. No game call in its
    // handler. Definitions.txt carries the same
    interface BodyRef {
        refrId: number;
        formType: number;
    }
    interface PropImpulseEvent {
        impact: number;
        kind: 'explosion' | 'impacts' | 'cone';
        cause: number;
        refrs: BodyRef[];
    }
    function on(eventName: 'propImpulse', callback: (event: PropImpulseEvent) => void): EventHandle;
    function once(eventName: 'propImpulse', callback: (event: PropImpulseEvent) => void): EventHandle;
    // a script's push onto a puppet, skipped by the platform and reported
    // (PushReportedEvent, Definitions.txt)
    interface PushReportedEvent {
        caster: number;
        target: number;
        force: number;
    }
    function on(eventName: 'pushReported', callback: (event: PushReportedEvent) => void): EventHandle;
    function once(eventName: 'pushReported', callback: (event: PushReportedEvent) => void): EventHandle;
    // another player's game's answer to a look this seat's script asked of
    // that player's figure (a question on the script channel), written into
    // the platform's look table by the player's server id and the target,
    // which answers the script's Actor.HasLOS there. An older platform has
    // none, and every use is guarded
    function setPlayerLook(player: number, target: number, los: boolean): boolean;

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
    // its first argument), run here and reported from here. Kind "forward" is
    // a state row's call this seat's engine skipped on a subject another seat
    // owns, from a stack only this seat runs, sent on for that owner to run;
    // since the one script channel it names its cause (1 a quest stage: its
    // quest and stage; 2 a conversation line, 3 a scene, 4 a package, 5 a
    // perk's or an interaction's event: its root's form and 0; 6 another
    // player's relayed entry or press: its subject and the relay's arm) and,
    // inside another player's acting scope, acting, that player's server id.
    // Kind "question" is Actor.HasLOS on another player's figure inside that
    // player's acting scope, a read only that player's own game can make:
    // self 0x14, args [{ formId }] the target, acting that player's server
    // id, sent for the server to ask that game
    interface ScriptCall {
        cls: string;
        fn: string;
        kind: "action" | "state" | "everyone" | "touch" | "look" | "created" | "forward" | "question";
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
        // thuum ADR-073 decision 13, the spell channel (#54): kind "created" is
        // a spell's PlaceAtMe on this seat's own player, or a call on what
        // such a placement returned, by this seat's own ids; placed is a
        // creation's result (0xff000000 and up), localArgs the arguments that
        // name a created reference, bit i for argument i
        placed?: number;
        localArgs?: number;
        cause?: { kind: number; form: number; n: number };
        acting?: number;
        // an owned latent action's call the seam answered at once (the
        // reference's 3D not loaded on this seat, or another player's press
        // started its script); absent from every other call and from an
        // older platform
        answered?: boolean;
    }
    interface ScriptCallsEvent {
        calls: ScriptCall[];
    }
    // the seam's census every 30 s, one row per counted kind of call: "Class.Name
    // (platform)", " (not hosted)", " (local)", " (unrelayable)", " (unresolved)",
    // " (frame mismatch)", " (skipped)" (the scriptNonOwnerSkip lever, off by default),
    // " (effect)" (train 52: a PlaceAtMe on a self this seat does not own, run inside
    // this seat's own player's effect, its result a local helper; and an actor's look
    // row on a game-file actor this seat does not simulate, run inside an effect this
    // seat's own player casts, queued in scriptCalls for the server to judge),
    // " (effect ended)" (an actor's look row on a dead game-file actor this seat does
    // not simulate, run by an ActiveMagicEffect script whose effect has ended, its
    // handle the VM's empty one, queued in scriptCalls for the server to judge:
    // vanilla ReanimateAshPile's TurnToAsh after its Waits) and " (helper)" (a
    // call on such a helper, run here and crossing nowhere),
    // the bare "Class.Name" (an owned self's state row, run here) and "Class.Name on
    // <hex id>" or " on others" (an everyone row's selves); apart from the two effect
    // look kinds and a trap trigger's " (look)", a call in scriptCalls is never
    // counted here. No game call in its handler.
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
    // tracker #111: the other end of those fields as the actor's last
    // movement step left them, the engine's own for an actor this game
    // simulates, the relay's feed for a puppet (null: no sample, or a
    // platform without the step's detour)
    function getActorLocomotion(actorFormId: number): { speed: number; directionDeg: number; turnRateDeg: number; ageMs: number } | null;
    function getHighProcessActors(): number[];
    // thuum tracker #121: the cells the actor's running package names as its
    // location, the game thread's latest snapshot (PuppetService's demote)
    function getPackageDestinationCells(actorFormId: number): number[];
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
    // id, which SendTriggerEvent takes), -4 a relayed activation (questId then
    // the pressed reference's form id, which ArmActivation takes; tracker
    // #117); figureLocalId the acting player's
    // figure here, 0 when this seat holds none; serverId its server id; false,
    // nothing stored, for a quest 0, a stage outside -4 to 65535, a serverId 0
    // or 0x14 as the figure. Inside that scope Game.GetPlayer answers the
    // figure. getActingPlayer answers the server id of the scope that
    // requested the pair here, 0 for none. setPlayerGlobals (PQ-3) names the
    // globals each player keeps (PlayerGlobalsService's watch), replaced whole
    // ([] clears): inside another player's acting scope a script's
    // GlobalVariable.SetValue on one is skipped here. The platform layer
    // writes the bodies on its own branch; an older platform has none, and
    // every use is guarded
    // since tracker #117 an activation's (-4) request may carry items, the
    // presser's recorded inventory as [baseId, count, ...], which the scope
    // answers GetItemCount on the figure from (Eli's design B)
    function setActingPlayer(questId: number, stage: number, figureLocalId: number, serverId: number, ttlMs: number, items?: number[]): boolean;
    // train 53, V-scene (m4-train-53-platform b29ea207, Definitions.txt): a
    // relay's stage (0 to 65535), start (-1) or stop (-2) whose acting player
    // is this seat's own or unnamed, scoped at the relay with no swap; false,
    // nothing stored, for a quest 0, a stage outside -2 to 65535 or a ttl
    // outside 1 to 60000; an older platform has none, and its use is guarded
    function setRelayedScope(questId: number, stage: number, ttlMs: number): boolean;
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
    // the world's active list; the body the root's own, else the first one on
    // the root's children (a trap rock's); an id with no reference, 3D, cell
    // world or rigid body on its root or a child of it is left out. With all
    // false a body is answered while awake and once more at the read its awake
    // bit fell (rule 9), with all true every body found. setBodyVelocity
    // queues the take-over's velocities to the same body on the main thread
    // (true when queued) and its task logs "[verb:prop-physics] velocity set
    // on <id>" or "velocity not set on <id>" with why. Definitions.txt
    // carries the same.
    // Since ADR-071 rung 4 the bodies are one walk of the whole 3D in the
    // scene graph's order (the node's own body, then its children's in array
    // order, depth first): a whole reference (one body, not Fixed: an item, a
    // trap rock) answers its one body at any depth, one with no part (an
    // all-Fixed static) its first body, motion 5, and one with a part (a body
    // that is not Invalid, Fixed or Character: a sign's boards on their Fixed
    // anchor) answers awake when any part is, motion, lin and ang the fastest
    // part's, and bones, every part's pose in walk order (BodyBone), absent
    // when the walk passed 64 parts, 512 nodes or 32 levels; pos and rot the
    // root's either way; an id whose walk faulted is left out too (said in
    // the platform's log). setBodyVelocity sets a whole body's alone: its
    // task refuses one of several bodies, "velocity not set on <id>: a body
    // of several parts (imposeBodies drives it)"
    interface BodyRead { formId: number; awake: boolean; motion: number; pos: number[]; rot: number[]; lin: number[]; ang: number[]; bones?: BodyBone[] }
    function readBodies(formIds: number[], all: boolean): BodyRead[];
    function setBodyVelocity(formId: number, lin: number[], ang: number[]): boolean;
    // physics rung 3 (the island's claim): with islands true an awake body of
    // a dynamic type also carries its island, the island's other dynamic
    // members (each reference once, at most 16, never the player's own or the
    // body's own, none for the world's fixed island), read on the JS thread
    // with no engine lock; absent when that read faulted and on every other
    // body. setImpulseCauseRule is the impulse rule's switch (off by default
    // in the platform; the client setting propImpulseCause's mirror), true
    // when ScaleGameplayImpulseForce's replacement went in at install
    interface BodyIslandRead extends BodyRead { island?: BodyRef[] }
    function readBodies(formIds: number[], all: boolean, islands: boolean): BodyIslandRead[];
    function setImpulseCauseRule(on: boolean): boolean;
    // thuum docs/DECISIONS.md ADR-071 rung 4 (D-PH9 A: each seat reads the
    // class from its loaded 3D): one part of a reference's 3D, a rigid body
    // whose motion type is not Invalid, Fixed or Character (1, 2, 3, 4 or 6):
    // node its node's name ("" for none), motion its motion type
    interface BodyPart { node: string; motion: number }
    // a reference's class as this seat's loaded 3D has it: bsx the 3D root's
    // BSX flags (0 with no block), dynamic their kDynamic bit (1 << 6),
    // bodies every rigid body under the root, fixed the Fixed ones, parts
    // each part in walk order (the scene graph's order: the node's own body,
    // then its children's in array order, depth first), cut a walk past 64
    // parts, 512 nodes or 32 levels (its counts partial). The client's
    // verdict: not dynamic, no part or cut is no row; one body and none Fixed
    // is a whole world body (the items' route); otherwise a world body of
    // parts.length parts
    // bones: an actor's ragdoll bones among parts (the first ones; a dead
    // actor's carried parts after them), 0 for any other root; ragdoll:
    // whether its engine holds an actor ragdolled now (thuum tracker #175)
    interface BodyClass { bsx: number; dynamic: boolean; bodies: number; fixed: number; parts: BodyPart[]; cut: boolean; bones: number; ragdoll: boolean }
    // the class read on the JS thread (the root's BSX block, then the walk),
    // no task, no log of its own; null with no reference, no 3D or a faulted
    // walk (never cache a null)
    function getBodyClass(formId: number): BodyClass | null;
    // D-PH10 A: one part's pose, readBodies' bones and imposeBodies' input:
    // pos the body origin's world position in game units, quat its rotation
    // as Havok keeps it, [x, y, z, w], unit within 0.01; a sample's bones
    // within 512 of its pos (the wire's reach)
    interface BodyBone { pos: number[]; quat: number[] }
    // the follower's impose, route K3 under D-PH26 A: the newest poses for a
    // reference's parts, bone i for this seat's walk's part i, stored on the
    // JS thread and laid on the parts by one drain on the main thread (SKSE's
    // UI task queue, never waited for): with rest, or a part more than 64
    // units from its pose, every part placed outright ("[verb:prop-physics]
    // impose <id> placed: part <i> <d> units off" for the latter, at most
    // once a second; a rest then drops the form), otherwise every part driven
    // by velocity toward its pose over its world's step, so each part stays a
    // live Havok body; a count other than the walk's parts imposes nothing
    // ("impose <id> skipped: <n> poses against <m> parts here", once), a form
    // with no 3D is dropped ("impose <id>: no reference with 3D, dropped");
    // the first drain logs "impose drain on thread <tid>, Main's <tid>". True
    // when stored; false for 0, the player's own id, no pose, more than 64, a
    // value not a number, a quat off unit length by more than 0.01, or 64
    // forms already held; a bone of another shape throws. With velocities,
    // one a bone, each driven part's target moves on by its velocity for the
    // time between the store and the drain, and the drive adds the velocity
    // to its correction (feedforward); another count, a value not a number,
    // or a speed past 6000 game units or 1000 radians a second refuses the
    // store. A platform before the argument ignores it
    interface BodyVelocity { lin: number[]; ang: number[] }
    function imposeBodies(formId: number, bones: BodyBone[], rest: boolean, velocities?: BodyVelocity[] | null): boolean;
    // the form's stored poses dropped, so the drain lays no more on it (a
    // take-over, a quiet stream, a follow's end); under K3 nothing else, the
    // parts' motion types never changed; true when poses were held, false
    // for 0, the player's own id or none held; a table operation on the JS
    // thread, no game call. The get-up hold is lifted with it (thuum tracker
    // #178); with place, a living ragdolled actor is first set there, then
    // its hold lifted, in one task on the main thread; with apex too (game
    // units, the flight's highest root), its character controller's fall
    // start is set from it between the two, so its own get-up counts the fall
    function releaseBodies(formId: number, place?: number[], apex?: number): boolean;
    // D-PH9 A (the native kill switch): the contact hook's world-body switch,
    // off by default (rung 3's filter: layers 4 and 10 at any motion); on, the
    // player's contact with a body of a dynamic motion type on any layer
    // raises propContact too; the client setting propWorldBodies' mirror;
    // logs "[verb:prop-physics] world-body contacts on" or "off"; stored with
    // no game call; true
    function setWorldBodyContacts(on: boolean): boolean;
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
    // FURN's own graph event outside a script action, in a platform call or
    // not, raised on a tick task at the animation hook's leave with the graph's
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
    // thuum docs/DECISIONS.md ADR-073 step 5a (decision 6 with D-L2 B and
    // D-A2 C): a Scene.Start (forced false) or ForceStart (forced true) a
    // script called on this seat, read at the platform's native dispatch
    // (ScriptNatives.cpp NoteSceneStart) and raised on a tick task: the
    // scene's form id, and the player whose seat's start it is (its server
    // id; 0 for this seat's own start). The platform raises only a start that
    // runs here, each this seat's own (0): one under a relay's root is skipped
    // there (train 53's V-scene). No game call in its handler. An older
    // platform has no such event and refuses the subscription, which
    // SceneClaimService guards (said once)
    interface SceneStartEvent {
        sceneId: number;
        forced: boolean;
        acting: number;
    }
    function on(eventName: 'sceneStart', callback: (event: SceneStartEvent) => void): EventHandle;
    function once(eventName: 'sceneStart', callback: (event: SceneStartEvent) => void): EventHandle;
    // thuum ADR-068 rung 5, D-Q4 C (the platform's QuestCause.h): a quest's
    // start, stop or stage with its cause as the seam read it at the call that
    // made it ('world' or 'seat') and the root frame of that call's stack
    // ("<Script>.<Function>", empty when no script call made it). The published
    // 2.8.0 types name the quest alone; an older platform raises neither
    // field, and QuestStageService reports as before when they are absent.
    // Optional here as in Definitions.txt, so the two merge
    // rootSelf (train 53, m4-train-53-platform's QuestCause::Verdict): the
    // root frame's self's form id, a scene fragment's scene, 0 for none
    interface QuestStartStopEvent {
        cause?: 'world' | 'seat';
        root?: string;
        rootSelf?: number;
    }
    interface QuestStageEvent {
        cause?: 'world' | 'seat';
        root?: string;
        rootSelf?: number;
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
    // own say with the info given; the result -1 pending, 0 no actor or
    // process, 1 started, 2 refused, 3 played by the sound descriptor path
    // (no state byte), 4 deferred, 5 inaudible, 6 unresolved here, 7 the
    // engine's pick took another info (tracker #123: the topic, the third
    // argument, is the key when the info does not resolve on this game)
    function sayTopicInfo(actorFormId: number, topicInfoFormId: number, topicFormId?: number): boolean;
    function sayTopicInfoResult(actorFormId: number): number;
    // thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the
    // engine): the server's cause applied through the engine's own damage
    // entry to an actor this game simulates, and the cut of this game's own
    // engine damage (npcEngineDamage's mirror)
    function damageActor(actorFormId: number, health: number, attackerFormId: number, killMove: boolean): boolean;
    // slice 5: the gaze and the face read on the game thread per poll ([target, slot, emotionType, emotionValue, exprOverride, exprIndex, exprValue])
    // own: true for this seat's own poll, whose table its sends read; absent
    // for any other reader (the lab's), which keeps a table of its own (#56)
    function pollHostedGaze(actorFormIds: number[], own?: boolean): void;
    function hostedGaze(actorFormId: number, own?: boolean): number[] | null;
    // thuum docs/verbs/npc-puppet.md (Ownership): the same poll's running
    // effects whose look the engine keeps on the actor, base effect form ids
    // ascending, each once; null when the last poll did not read the actor;
    // an older platform has none, and its use is guarded
    function hostedEffects(actorFormId: number, own?: boolean): number[] | null;
    function pollNodeRotation(refrId: number, nodeNames: string[]): void;
    function nodeRotation(refrId: number): Array<{ node: string; has: boolean; world: number[]; local: number[] }> | null;
    // thuum D-53-8 B (train 53's V-live): the head parts the player's own
    // base draws, read on the game thread per poll (its overlay list while
    // TESNPC::HasOverlays answers true and the list holds parts, else its
    // base list), and the last read; an older platform has neither
    function pollOwnHeadParts(): void;
    // thuum #63 and #47: a load door's other side, and an exterior cell's grid square, each read on the game thread and answered later
    function pollLinkedDoors(refrIds: number[]): void;
    function linkedDoorOf(refrId: number): number | null;
    function pollCellGrids(cellIds: number[]): void;
    function cellGridOf(cellId: number): { world: number; x?: number; y?: number } | null;
    function ownHeadParts(): { seq: number; race: number; flagged: boolean; hasOverlays: boolean; overlayCount: number; overlay: boolean; parts: number[] } | null;
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
    function resurrectActor(refrId: number, simulatedHere?: boolean): boolean;
    function redressActor(refrId: number): boolean;
    // thuum docs/verbs/npc-fate.md (playtest 48, the loot strip): a dead body's
    // record-worn armor that it holds unworn, equipped again after an apply
    function dressBody(refrId: number, worn: number[]): boolean;
    function actorFateCount(): number;
    // thuum docs/verbs/npc-fate.md C4 (ADR-060's return): the command of a
    // living thrall this game's player cast and simulates though its engine
    // holds no effect on it, one main-thread task that starts, refreshes or
    // ends it by the engine's state (FateApi.cpp CommandActor)
    function commandActor(refrId: number, on: boolean): boolean;

    // thuum docs/verbs/summon.md (tracker #84): the fork's Skyrim Platform
    // hooks SummonCreatureEffect's own Update and Finish and raises
    // summonStart once, on an update task, when the effect's actor has
    // appeared (every caster's), then summonEnd at the effect's Finish; a form
    // the game no longer holds is null (the engine's Finish deletes the
    // summon), its id kept. The published 2.8.0 typings name
    // neither event; these mirror the fork's Declarations.txt and the
    // generated skyrimPlatform.ts (its SummonStartEvent and SummonEndEvent,
    // lines 247-266)
    interface SummonStartEvent {
        caster: Actor;
        summon: Actor;
        effect: MagicEffect;
        // the spell that applied the effect, null when none is found
        spell: Form | null;
        casterId: number;
        summonId: number;
        // the active effect's own duration and elapsed seconds
        durationS: number;
        elapsedS: number;
    }
    interface SummonEndEvent {
        caster: Actor | null;
        summon: Actor | null;
        effect: MagicEffect | null;
        casterId: number;
        summonId: number;
    }
    function on(eventName: 'summonStart', callback: (event: SummonStartEvent) => void): EventHandle;
    function once(eventName: 'summonStart', callback: (event: SummonStartEvent) => void): EventHandle;
    function on(eventName: 'summonEnd', callback: (event: SummonEndEvent) => void): EventHandle;
    function once(eventName: 'summonEnd', callback: (event: SummonEndEvent) => void): EventHandle;

    // skyrim-platform's created-form natives (InventoryApi.cpp), which the
    // published 2.8.0 typings lack; these mirror its Definitions.txt and the
    // generated skyrimPlatform.ts (sync/createdForms.ts is their one caller)
    interface CreatedEffect {
        mgef: number;
        magnitude: number;
        area: number;
        duration: number;
        cost: number;
    }
    function readCreatedForm(formId: number): { kind: number; effects: CreatedEffect[] } | null;
    function createCreatedForm(kind: number, effects: CreatedEffect[]): number;
    function createdFormTemplate(kind: number): number;

    // The fork's Skyrim Platform hooks PlayerCharacter's own GoToPrison,
    // ServePrisonTime and crime gold setters (Hooks.cpp
    // InstallPlayerPrisonHooks) and raises these on update tasks; the
    // published 2.8.0 typings name none of them. They mirror the fork's
    // Definitions.txt and the generated skyrimPlatform.ts (ArrestEvent,
    // ServePrisonTimeEvent, CrimeGoldEvent)
    interface ArrestEvent {
        faction: Faction;
        factionId: number;
        // the sentence by the engine's own rule, 1 to the GMST's cap
        days: number;
    }
    interface ServePrisonTimeEvent {
        factionId: number;
        days: number;
    }
    interface CrimeGoldEvent {
        faction: Faction;
        factionId: number;
        paidGold?: number;
    }
    // PlayerCharacter's PickUpObject: the reference, the base and count the
    // player's inventory gained, the owner marked on the stack (0 for none)
    interface PickUpObjectEvent {
        refId: number;
        baseId: number;
        count: number;
        ownerId: number;
    }
    function on(eventName: 'arrest', callback: (event: ArrestEvent) => void): EventHandle;
    function once(eventName: 'arrest', callback: (event: ArrestEvent) => void): EventHandle;
    function on(eventName: 'servePrisonTime', callback: (event: ServePrisonTimeEvent) => void): EventHandle;
    function once(eventName: 'servePrisonTime', callback: (event: ServePrisonTimeEvent) => void): EventHandle;
    function on(eventName: 'crimeGold', callback: (event: CrimeGoldEvent) => void): EventHandle;
    function once(eventName: 'crimeGold', callback: (event: CrimeGoldEvent) => void): EventHandle;
    function on(eventName: 'pickUpObject', callback: (event: PickUpObjectEvent) => void): EventHandle;
    function once(eventName: 'pickUpObject', callback: (event: PickUpObjectEvent) => void): EventHandle;
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
