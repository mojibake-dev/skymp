// The natives table's overlay (thuum ADR-073 step 4; decision 5: the only hand
// data the table takes), included by natives_rules.rs. Each entry is decided
// (it cites the ADR row or D-number that sets its cells) or a stopgap (it
// names the rule that replaces it and the condition that deletes it), and
// sets only the cells the rules give otherwise; natives_rules::generate
// refuses any other. The live entries are the live table in its order, each
// with the comment natives_table.rs writes above its row. Since step 4's S4
// no entry sets an owner the rules ask (decision 6), and the hook cells hold
// what the platform's name lists held (natives::Hook, each code's decision 5
// line). Edit this file,
// then `cargo run -p wire-rules --bin natives-gen` writes the table;
// natives_rules.rs's generated-table test refuses a stale one.

/// Cited by ObjectReference.PlayAnimation, ObjectReference.PlayAnimationAndWait.
const ANIMATION: &str = "ADR-068 rung 2 (docs/research/scripts-one-system-2026-11-05.md 6.3): an action row, the cell's host's, its graph event kept as the subject's lastAnimation (rung 7 retires it)";
/// Cited by Debug.SendAnimationEvent.
const STATIC_ANIMATION: &str = "ADR-068 rung 2 (docs/research/scripts-one-system-2026-11-05.md 6.3): an action row on a static's first argument, the cell's host's";
/// Cited by ObjectReference.SetAnimationVariableBool, ObjectReference.SetAnimationVariableInt, ObjectReference.SetAnimationVariableFloat.
const VARIABLE: &str = "ADR-068 rung 2 (docs/research/scripts-one-system-2026-11-05.md 6.3): an action row, the cell's host's; its variable kept on its subject by the fifty fixes' D-C3 A (accepted by Eli 2026-11-07, thuum docs/DECISIONS.md:4953; docs/research/fifty-fixes-2026-11-07.md section 1(c))";
/// Cited by ObjectReference.AddItem, ObjectReference.RemoveItem, ObjectReference.GetItemCount.
const OWN_INVENTORY: &str = "Eli's D-R1 C (thuum docs/DECISIONS.md, rung 5's stage-eve words): the player's own inventory, its own seat's, kept as its own record";
/// Cited by ObjectReference.Enable, ObjectReference.EnableNoWait, ObjectReference.Disable, ObjectReference.DisableNoWait, ObjectReference.Delete.
const REF_STATE: &str = "ADR-068 rung 4, amendment B (docs/research/rung4-state-rows-2026-11-07.md section 1(a)): the ref state's record, the cell's host's, never relayed as an action";
/// Cited by ObjectReference.PlaceAtMe, ObjectReference.PlaceActorAtMe.
const PLACEMENT: &str = "ADR-068 rung 4, amendment B (the same plan's section 1(a)): the placements' record, the cell's host's, a state row until ADR-073 decision 13's placement";
/// Cited by ObjectReference.MoveTo, ObjectReference.SetOpen, ObjectReference.Lock, ObjectReference.SetLockLevel, ObjectReference.Activate, ObjectReference.SetDestroyed, ObjectReference.DamageObject.
const FLIP_OBJECT: &str = "ADR-068 rung 2's flip (6.3): a world object's state row, the cell's host's, never relayed as an action";
/// Cited by Actor.Kill, Actor.KillSilent, Actor.Resurrect.
const FLIP_ACTOR: &str = "ADR-068 rung 2's flip (6.3): an actor's state row, its simulator's, never relayed as an action";
/// Cited by Quest.SetCurrentStageID, Quest.Start, Quest.Stop, GlobalVariable.SetValue, ReferenceAlias.ForceRefTo.
const NOBODYS: &str = "ADR-068 rung 5's flip and D-F1 C: nobody's, every seat runs it";
/// Cited by Game.GetPlayer.
const SEATS_OWN: &str = "ADR-068 rung 2 (docs/research/scripts-one-system-2026-11-05.md 6.3) and rung 5's acting scope: the seat's own, never sent";
/// Cited by ObjectReference.ProcessTrapHit.
const TRAP_HIT: &str = "ADR-068 D-S3 point 2 (thuum docs/verbs/npc-fate.md, playtest 48): a trap's hit, the hit actor's simulator's, kept as its death evidence";
/// Cited by ObjectReference.SetMotionType, ObjectReference.ApplyHavokImpulse.
const TRAP_MOTION: &str = "ADR-068 D-S3 point 4 and D-W2 A (docs/research/fifty-fixes-2026-11-07.md section 7.2): a trap body's motion, its toucher's, a firing's look alone";
/// Cited by Actor.SetFactionRank, Actor.ModFactionRank, Actor.RemoveFromFaction, Actor.RemoveFromAllFactions, Actor.SetCrimeFaction, Actor.SetRelationshipRank.
const STANDING: &str = "ADR-068 rung 4, amendment B (docs/research/rung4-state-rows-2026-11-07.md section 1(a)): an NPC's standing, its simulator's, kept as its record";
/// Cited by ReferenceAlias.Clear.
const ALIAS_CLEAR: &str =
    "ADR-068 rung 4 and D-F1 C: nobody's, beside ForceRefTo, every seat runs it";
/// Cited by Sound.Play.
const TRIGGER_SOUND: &str = "ADR-068 D-W2 A (docs/research/fifty-fixes-2026-11-07.md section 7.2(b)): a trigger's sound in its firing, the cell's host's, a look alone";
/// Cited by ObjectReference.CreateDetectionEvent.
const TRIGGER_NOISE: &str = "ADR-068 D-W2 A (the same plan's section 7.6): a trigger's noise in its firing, a look alone, relayed to the cell's host";
/// Cited by Actor.SetRace, Actor.AddSpell, Actor.RemoveSpell, Actor.AddShout, Actor.RemoveShout, Actor.EquipItem.
const PLAYER_OWN: &str = "ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md section 8(b)): the player's own, its own seat's, kept as its own record";
/// Cited by Actor.ChangeHeadPart.
const HEAD_PART: &str = "Eli's D-E1 A (ADR-068 rung 5): the player's own head part, its own seat's, kept as its own record";
/// The spec's New 1 stopgap's replacing rule.
const SINK: &str = "D2's singleton sink evidence (the spec's New 1 and G8): a native that writes the player singleton's own state takes the acting seat's skip by its sink, never by its name";
/// Its deleting condition.
const SINK_DELETES: &str = "when x-native-effects reads the native's sink and the rule keys on it (the row's evidence a trace)";

/// The scene starts' stopgap (train 53's V-scene hook): its replacing rule.
const SCENE_CLAIM: &str = "step 5a's scene row (ADR-073 decision 6, D-L2 B: the seat that started a scene, the stage's cause seat by D-A2 C, claims its actors for its life)";
/// Its deleting condition.
const SCENE_CLAIM_DELETES: &str = "at 5a's scene row, when the cause seat's start claims the scene and the other seats skip theirs";

/// AddToMap's quest marker (step 4's S6, Eli's D-NT4 A): its replacing rule.
const QUEST_MARKERS: &str = "step 1's quest record (ADR-073 decision 2 (c); D-G6 A: the markers are the quest record's): a quest-owned marker kept on its quest's record and shown to everyone the quest is shown to";
/// Its deleting condition.
const QUEST_MARKERS_DELETES: &str = "when step 1's quest record holds the quest's markers and serves them at a login and to a late joiner";

/// An actor's look rows (the Undead FX build, merged over step 4).
const ACTOR_LOOK: &str = "ADR-060's amendment of 2026-11-12 (the Undead FX build; the reanimate-fx memo's D6 B and D3 B) and ADR-068 decision 6: an actor's own look, which the script its simulator runs makes, relayed to its listeners as the same native; its owner asked (ADR-073 decision 6), the actor's simulator, or a dead thrall's reanimate caster for the ash window";
/// The five a watcher keeps (thuum #52).
const ACTOR_LOOK_KEPT: &str = "ADR-060's amendment of 2026-11-12 and thuum #52 (the review of the client's 3D keep, eaa6ba47, findings 1 and 2): an actor's own look, its simulator's, relayed as the same native and kept as the actor's look (wire-rules actor_looks), served with its create word";

/// Every overlay entry: the live table's sixty-one rows in its order (rung
/// 5's fifty, the two scene starts, AddToMap's quest marker, then the Undead
/// FX build's eight look rows of an actor), then the derived rows' decided
/// cells and stopgaps.
pub const OVERLAY: &[Entry] = &[
    Entry {
        native: "ObjectReference.PlayAnimation",
        class: Class::Decided { cites: ANIMATION },
        live: true,
        note: &[
            "the actions: relayed to the subject's listeners as the row's relay",
            "native, the graph variables' kept on the subject as well",
        ],
        cells: &[Cell::Keep(Keep::Last)],
    },
    Entry {
        native: "ObjectReference.PlayAnimationAndWait",
        class: Class::Decided { cites: ANIMATION },
        live: true,
        note: &[],
        cells: &[Cell::Keep(Keep::Last)],
    },
    Entry {
        native: "Debug.SendAnimationEvent",
        class: Class::Decided { cites: STATIC_ANIMATION },
        live: true,
        note: &[],
        cells: &[],
    },
    Entry {
        native: "ObjectReference.SetAnimationVariableBool",
        class: Class::Decided { cites: VARIABLE },
        live: true,
        note: &[],
        cells: &[Cell::Keep(Keep::Variable)],
    },
    Entry {
        native: "ObjectReference.SetAnimationVariableInt",
        class: Class::Decided { cites: VARIABLE },
        live: true,
        note: &[],
        cells: &[Cell::Keep(Keep::Variable)],
    },
    Entry {
        native: "ObjectReference.SetAnimationVariableFloat",
        class: Class::Decided { cites: VARIABLE },
        live: true,
        note: &[],
        cells: &[Cell::Keep(Keep::Variable)],
    },
    Entry {
        native: "ObjectReference.AddItem",
        class: Class::Decided { cites: OWN_INVENTORY },
        live: true,
        note: &[
            "the flip's state rows: the object's record carries them (rung 4: the",
            "ref state's five and the placements' two kept as their subject's",
            "record, the rest still carried by the change form or a typed verb).",
            "Eli's D-R1 C: a script's AddItem and RemoveItem on the seat's own",
            "player (its subject 0x14) are that player's own inventory record, R2,",
            "judged by wire-rules player_own with no bound (a quest's reward fires",
            "every time, a leveled one or a count its script computes among them);",
            "on any other subject they are the counted state rows they were, with",
            "no channel (record_owner's E_SCRIPTCALL_ROW)",
        ],
        cells: &[Cell::Keep(Keep::Record(RecordKind::PlayerOwn)), Cell::Hook(Hook::LeveledLanding)],
    },
    Entry {
        native: "ObjectReference.RemoveItem",
        class: Class::Decided { cites: OWN_INVENTORY },
        live: true,
        note: &[],
        cells: &[Cell::Keep(Keep::Record(RecordKind::PlayerOwn))],
    },
    Entry {
        native: "ObjectReference.Enable",
        class: Class::Decided { cites: REF_STATE },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::RefState))],
    },
    Entry {
        native: "ObjectReference.EnableNoWait",
        class: Class::Decided { cites: REF_STATE },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::RefState))],
    },
    Entry {
        native: "ObjectReference.Disable",
        class: Class::Decided { cites: REF_STATE },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::RefState))],
    },
    Entry {
        native: "ObjectReference.DisableNoWait",
        class: Class::Decided { cites: REF_STATE },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::RefState))],
    },
    Entry {
        native: "ObjectReference.Delete",
        class: Class::Decided { cites: REF_STATE },
        live: true,
        note: &[],
        cells: &[Cell::Keep(Keep::Record(RecordKind::RefState)), Cell::Hook(Hook::HelperDelete)],
    },
    Entry {
        native: "ObjectReference.PlaceAtMe",
        class: Class::Decided { cites: PLACEMENT },
        live: true,
        note: &[],
        cells: &[Cell::Kind(natives::Kind::State), Cell::Hook(Hook::EffectHelper)],
    },
    Entry {
        native: "ObjectReference.PlaceActorAtMe",
        class: Class::Decided { cites: PLACEMENT },
        live: true,
        note: &[],
        cells: &[Cell::Kind(natives::Kind::State)],
    },
    Entry {
        native: "ObjectReference.MoveTo",
        class: Class::Decided { cites: FLIP_OBJECT },
        live: true,
        note: &[],
        cells: &[],
    },
    Entry {
        native: "ObjectReference.SetOpen",
        class: Class::Decided { cites: FLIP_OBJECT },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never)],
    },
    Entry {
        native: "ObjectReference.Lock",
        class: Class::Decided { cites: FLIP_OBJECT },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never)],
    },
    Entry {
        native: "ObjectReference.SetLockLevel",
        class: Class::Decided { cites: FLIP_OBJECT },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never)],
    },
    Entry {
        native: "ObjectReference.Activate",
        class: Class::Decided { cites: FLIP_OBJECT },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never)],
    },
    Entry {
        native: "ObjectReference.SetDestroyed",
        class: Class::Decided { cites: FLIP_OBJECT },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Hook(Hook::TrapDestroyed)],
    },
    Entry {
        native: "ObjectReference.DamageObject",
        class: Class::Decided { cites: FLIP_OBJECT },
        live: true,
        note: &[],
        cells: &[],
    },
    Entry {
        native: "Actor.Kill",
        class: Class::Decided { cites: FLIP_ACTOR },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Hook(Hook::DeathReport)],
    },
    Entry {
        native: "Actor.KillSilent",
        class: Class::Decided { cites: FLIP_ACTOR },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Hook(Hook::DeathReport)],
    },
    Entry {
        native: "Actor.Resurrect",
        class: Class::Decided { cites: FLIP_ACTOR },
        live: true,
        note: &[],
        cells: &[],
    },
    Entry {
        native: "Quest.SetCurrentStageID",
        class: Class::Decided { cites: NOBODYS },
        live: true,
        note: &[
            "the flip's everyone rows: nobody owns them, every seat runs them (rung 5)",
        ],
        cells: &[Cell::Hook(Hook::QuestStage)],
    },
    Entry {
        native: "Quest.Start",
        class: Class::Decided { cites: NOBODYS },
        live: true,
        note: &[],
        cells: &[Cell::Hook(Hook::QuestStart)],
    },
    Entry {
        native: "Quest.Stop",
        class: Class::Decided { cites: NOBODYS },
        live: true,
        note: &[],
        cells: &[Cell::Hook(Hook::QuestStop)],
    },
    Entry {
        native: "GlobalVariable.SetValue",
        class: Class::Decided { cites: NOBODYS },
        live: true,
        note: &[],
        cells: &[Cell::Hook(Hook::PlayerGlobal)],
    },
    Entry {
        native: "ReferenceAlias.ForceRefTo",
        class: Class::Decided { cites: NOBODYS },
        live: true,
        note: &[],
        cells: &[Cell::Hook(Hook::AliasFill)],
    },
    Entry {
        native: "Game.GetPlayer",
        class: Class::Decided { cites: SEATS_OWN },
        live: true,
        note: &[
            "local: the seat's own (rung 5 adds the acting player's scope)",
        ],
        cells: &[Cell::Hook(Hook::ActingReturn)],
    },
    Entry {
        native: "ObjectReference.ProcessTrapHit",
        class: Class::Decided { cites: TRAP_HIT },
        live: true,
        note: &[
            "ADR-068 D-S3 (traps follow the interaction): a trap's hit is the hit",
            "actor's own seat's (its self, the hit actor, owned there alone, so a",
            "non-owner's copy skips it), and a trap body's motion is the seat whose",
            "firing moved it; appended so no earlier row's place moves. The hit's",
            "owner reports it, kept as the actor's death evidence (Keep::Evidence;",
            "thuum docs/verbs/npc-fate.md, playtest 48: a blade kill after its",
            "trigger's 30 s was refused)",
        ],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Evidence)],
    },
    Entry {
        native: "ObjectReference.SetMotionType",
        class: Class::Decided { cites: TRAP_MOTION },
        live: true,
        note: &[],
        cells: &[Cell::Look(Look::Only), Cell::Hook(Hook::TrapMotion)],
    },
    Entry {
        native: "ObjectReference.ApplyHavokImpulse",
        class: Class::Decided { cites: TRAP_MOTION },
        live: true,
        note: &[],
        cells: &[Cell::Look(Look::Only), Cell::Hook(Hook::TrapImpulse)],
    },
    Entry {
        native: "Actor.SetFactionRank",
        class: Class::Decided { cites: STANDING },
        live: true,
        note: &[
            "ADR-068 rung 4 (the state rows, amendment B): an NPC's standing, its",
            "simulator's, kept as its record (W3: the server's half and T2 this",
            "rung, no observe point on a seat; the player's own 0x14 refused until",
            "rung 5), in docs/research/rung4-state-rows-2026-11-07.md's order;",
            "AddToFaction is Papyrus over SetFactionRank, no native of its own. Then",
            "the alias's clear, nobody's like ForceRefTo (D-F1 C), which every seat",
            "runs and AliasFill 79 carries. Appended so no earlier row's place moves",
        ],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::Standing))],
    },
    Entry {
        native: "Actor.ModFactionRank",
        class: Class::Decided { cites: STANDING },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::Standing))],
    },
    Entry {
        native: "Actor.RemoveFromFaction",
        class: Class::Decided { cites: STANDING },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::Standing))],
    },
    Entry {
        native: "Actor.RemoveFromAllFactions",
        class: Class::Decided { cites: STANDING },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::Standing))],
    },
    Entry {
        native: "Actor.SetCrimeFaction",
        class: Class::Decided { cites: STANDING },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::Standing))],
    },
    Entry {
        native: "Actor.SetRelationshipRank",
        class: Class::Decided { cites: STANDING },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::Standing))],
    },
    Entry {
        native: "ReferenceAlias.Clear",
        class: Class::Decided { cites: ALIAS_CLEAR },
        live: true,
        note: &[],
        cells: &[Cell::Hook(Hook::AliasFill)],
    },
    Entry {
        native: "Sound.Play",
        class: Class::Decided { cites: TRIGGER_SOUND },
        live: true,
        note: &[
            "ADR-068 D-W2 A (Eli 2026-11-07; docs/research/fifty-fixes-2026-11-07.md",
            "section 7.2): a trap family trigger's sound during its firing, a look",
            "alone, never an action on its own (outside a look the platform counts",
            "it local and queues nothing); its subject is its source, the first",
            "argument, and its self the Sound (Subject::Arg0Method); FunctionsDump's",
            "Play(ObjectReference akSource), a member native returning the instance.",
            "Appended so no earlier row's place moves",
        ],
        cells: &[Cell::Look(Look::Only)],
    },
    Entry {
        native: "ObjectReference.CreateDetectionEvent",
        class: Class::Decided { cites: TRIGGER_NOISE },
        live: true,
        note: &[
            "the trigger script's CreateDetectionEvent during its firing (SoundTrap's",
            "op 23 on itself; the same plan's section 7.6, the coordinator's call",
            "under Eli's condition): a look alone, relayed to the cell's host alone",
            "(the NPCs' simulator), its akOwner the reporter's figure where the call",
            "named the reporter's own player, so a guest's bone alarm still wakes",
            "the NPCs the host simulates; FunctionsDump's CreateDetectionEvent(Actor",
            "akOwner, Int aiSoundLevel). Appended so no earlier row's place moves",
        ],
        cells: &[Cell::Kind(natives::Kind::Action), Cell::Audience(Audience::Host), Cell::Look(Look::Only)],
    },
    Entry {
        native: "Actor.SetRace",
        class: Class::Decided { cites: PLAYER_OWN },
        live: true,
        note: &[
            "ADR-068 rung 5 (Eli's \"yes full transformation\"; docs/research/",
            "rung5-quests-2026-11-09.md section 8(b)): the six natives a per-player",
            "quest's scripts change their own player by (the vampire's twin, cure",
            "and stage abilities, the werewolf's beast form, its abilities, howl",
            "and skin), each a state row on the seat's own player alone",
            "(Owner::Player, its subject 0x14), kept as the player's own record and",
            "judged by wire-rules player_own (the bound, the twin rule, the",
            "overlay); FunctionsDump's SetRace(Race akRace), AddSpell(SPELL",
            "akSpell, Bool abVerbose), RemoveSpell(SPELL akSpell), AddShout(Shout",
            "akShout), RemoveShout(Shout akShout) and EquipItem(Form akItem, Bool",
            "abPreventRemoval, Bool abSilent). Appended so no earlier row's place",
            "moves",
        ],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::PlayerOwn))],
    },
    Entry {
        native: "Actor.AddSpell",
        class: Class::Decided { cites: PLAYER_OWN },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::PlayerOwn))],
    },
    Entry {
        native: "Actor.RemoveSpell",
        class: Class::Decided { cites: PLAYER_OWN },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::PlayerOwn))],
    },
    Entry {
        native: "Actor.AddShout",
        class: Class::Decided { cites: PLAYER_OWN },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::PlayerOwn))],
    },
    Entry {
        native: "Actor.RemoveShout",
        class: Class::Decided { cites: PLAYER_OWN },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::PlayerOwn))],
    },
    Entry {
        native: "Actor.EquipItem",
        class: Class::Decided { cites: PLAYER_OWN },
        live: true,
        note: &[],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::PlayerOwn))],
    },
    Entry {
        native: "Actor.ChangeHeadPart",
        class: Class::Decided { cites: HEAD_PART },
        live: true,
        note: &[
            "thuum ADR-068 rung 5, Eli's D-E1 A: a seventh player's own row in the",
            "six's shape, a script's swap of its own player's head part (rotfern's",
            "eyes, RotfernEyes for RotfernEyesMora), judged by wire-rules player_own",
            "(the bound, the head part valid for the recorded race) and recorded on",
            "the look; FunctionsDump's ChangeHeadPart(HeadPart hPart). Appended so",
            "no earlier row's place moves",
        ],
        cells: &[Cell::Relay(Relay::Never), Cell::Keep(Keep::Record(RecordKind::PlayerOwn))],
    },
    // train 53's V-scene (fork 1943abdc; ADR-073 D-A2 C): the two natives that
    // start a scene, whose call inside a scope rooted at a relay the seam
    // counts and runs until step 5a's scene claim, the hook that replaces the
    // platform's own list of them (step 4's S4); creations, judged as state
    // rows, their row never sent
    Entry {
        native: "Scene.Start",
        class: Class::Stopgap { replaced_by: SCENE_CLAIM, deletes: SCENE_CLAIM_DELETES },
        live: true,
        note: &[
            "train 53's V-scene (ADR-073 D-A2 C, the stage's cause seat owns a",
            "scene a fragment starts): Scene's Start() and ForceStart(), creations",
            "judged as state rows and never sent, whose call inside a scope rooted",
            "at a relay the platform counts and runs (their hook) until step 5a's",
            "scene row claims the scene's actors for its starting seat (D-L2 B).",
            "Appended so no earlier row's place moves",
        ],
        cells: &[Cell::Hook(Hook::SceneStart)],
    },
    Entry {
        native: "Scene.ForceStart",
        class: Class::Stopgap { replaced_by: SCENE_CLAIM, deletes: SCENE_CLAIM_DELETES },
        live: true,
        note: &[],
        cells: &[Cell::Hook(Hook::SceneStart)],
    },
    // step 4's S6 (Eli's D-NT4 A; D-PO1 B and D-G6 A): a quest script's
    // AddToMap, the map-markers record's report from every seat that runs it,
    // a stopgap until step 1's quest record holds the quest's own markers
    Entry {
        native: "ObjectReference.AddToMap",
        class: Class::Stopgap { replaced_by: QUEST_MARKERS, deletes: QUEST_MARKERS_DELETES },
        live: true,
        note: &[
            "ADR-073 step 4's S6 (Eli's D-NT4 A; D-PO1 B, D-G6 A): a quest script's",
            "AddToMap reveals its marker on the map of everyone the quest is shown",
            "to. An everyone row every seat that runs it reports, judged by its",
            "cause (a quest's, a topic info's or a scene's script whose quest the",
            "server holds live) and its subject (a map marker by the master files),",
            "recorded on the map-markers record (thuum docs/verbs/map-markers.md)",
            "and relayed live as AddToMap. Appended so no earlier row's place moves",
        ],
        cells: &[
            Cell::Kind(natives::Kind::Everyone),
            Cell::Owner(natives::Owner::Nobody),
            Cell::Audience(Audience::Everyone),
            Cell::Relay(Relay::Never),
            Cell::Keep(Keep::Record(RecordKind::Marker)),
        ],
    },
    Entry {
        native: "EffectShader.Play",
        class: Class::Decided { cites: ACTOR_LOOK_KEPT },
        live: true,
        note: &[
            "the Undead FX build (thuum ADR-060's amendment of 2026-11-12; the",
            "reanimate-fx memo's D6 B and D3 B): an actor's own looks, which the",
            "script its simulator runs makes (decision 6), relayed to its listeners",
            "as the same native; an effect form's subject is its first argument and",
            "its self the form (Subject::Arg0Method). Kept as the actor's look and",
            "served with its create word (thuum #52). Appended so no earlier row's",
            "place moves",
        ],
        cells: &[Cell::Keep(Keep::Look(natives::LookKeep::ShaderPlay)), Cell::Look(Look::Never)],
    },
    Entry {
        native: "EffectShader.Stop",
        class: Class::Decided { cites: ACTOR_LOOK_KEPT },
        live: true,
        note: &[
        ],
        cells: &[Cell::Keep(Keep::Look(natives::LookKeep::ShaderStop)), Cell::Look(Look::Never)],
    },
    Entry {
        native: "VisualEffect.Play",
        class: Class::Decided { cites: ACTOR_LOOK_KEPT },
        live: true,
        note: &[
        ],
        cells: &[Cell::Keep(Keep::Look(natives::LookKeep::EffectPlay)), Cell::Look(Look::Never)],
    },
    Entry {
        native: "VisualEffect.Stop",
        class: Class::Decided { cites: ACTOR_LOOK_KEPT },
        live: true,
        note: &[
        ],
        cells: &[Cell::Keep(Keep::Look(natives::LookKeep::EffectStop)), Cell::Look(Look::Never)],
    },
    Entry {
        native: "Actor.SetEyeTexture",
        class: Class::Decided { cites: ACTOR_LOOK_KEPT },
        live: true,
        note: &[
        ],
        cells: &[Cell::Kind(natives::Kind::Action), Cell::Audience(Audience::Listeners), Cell::Keep(Keep::Look(natives::LookKeep::Eyes))],
    },
    Entry {
        native: "Actor.SetAlpha",
        class: Class::Decided { cites: ACTOR_LOOK },
        live: true,
        note: &[
            "vanilla ReanimateAshPile's TurnToAsh: the disintegrate shader above, then",
            "SetAlpha, SetCriticalStage and AttachAshPile, a one-shot each, kept",
            "nowhere (a replay at every 3D load would repeat it)",
        ],
        cells: &[Cell::Kind(natives::Kind::Action), Cell::Audience(Audience::Listeners)],
    },
    Entry {
        native: "Actor.SetCriticalStage",
        class: Class::Decided { cites: ACTOR_LOOK },
        live: true,
        note: &[
        ],
        cells: &[Cell::Kind(natives::Kind::Action), Cell::Audience(Audience::Listeners)],
    },
    Entry {
        native: "Actor.AttachAshPile",
        class: Class::Decided { cites: ACTOR_LOOK },
        live: true,
        note: &[
        ],
        cells: &[Cell::Kind(natives::Kind::Action), Cell::Audience(Audience::Listeners)],
    },
    // ADR-062's BlockActivation row (DECISIONS.md:4020, "no hook
    // (host-local)": the engine's own gate on each seat), a declared per-seat
    // cell (ADR-073 decision 5 (a); the spec's New 3)
    Entry {
        native: "ObjectReference.BlockActivation",
        class: Class::Decided { cites: "ADR-062 (DECISIONS.md:4020): no hook, host-local, the engine's own gate on each seat; a declared per-seat cell (ADR-073 decision 5 (a))" },
        live: false,
        note: &[],
        cells: &[Cell::Kind(natives::Kind::Local), Cell::Owner(natives::Owner::Player), Cell::Relay(Relay::Never)],
    },
    // the spec's New 1: the player's own state written by a member native, which
    // inside another player's acting scope runs on the acting seat alone, as a
    // static on no object does by its subject; a stopgap until the native's
    // sink names it
    Entry {
        native: "Faction.SetPlayerEnemy",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "Faction.SetPlayerExpelled",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "Faction.PlayerPayCrimeGold",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "Faction.SendPlayerToJail",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "Faction.SetCrimeGold",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "Faction.ModCrimeGold",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "Form.SetPlayerKnows",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "Actor.SetPlayerResistingArrest",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "Quest.SetActive",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "ImageSpaceModifier.Apply",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "ImageSpaceModifier.ApplyCrossFade",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    Entry {
        native: "ImageSpaceModifier.PopTo",
        class: Class::Stopgap { replaced_by: SINK, deletes: SINK_DELETES },
        live: false,
        note: &[],
        cells: &[Cell::Scoped(Scoped::ActingSeatAlone)],
    },
    // the spec's New 3: ADR-062's weather row (DECISIONS.md:4029, "the host's
    // forced weather is a weather report") is step 3's to build; until then the
    // three weather natives run where their script runs, ForceActive and
    // SetActive by K7 and ReleaseOverride, a static on no object, by this entry
    // (a Known limit in docs/verbs/weather.md)
    Entry {
        native: "Weather.ReleaseOverride",
        class: Class::Stopgap {
            replaced_by: "ADR-062's weather report (DECISIONS.md:4029; the spec's New 3): the host's forced weather reported to step 3's R0 weather record",
            deletes: "when step 3's R0 weather record takes its owner's report (ADR-073 decision 12)",
        },
        live: false,
        note: &[],
        cells: &[Cell::Kind(natives::Kind::Everyone), Cell::Owner(natives::Owner::Nobody), Cell::Audience(Audience::Everyone), Cell::Scoped(Scoped::Runs)],
    },
    // thuum tracker #117 (playtest 53's Sanctum door; Eli's design B, "go with
    // b for 117"): the keyhole's HallofStoriesKeyholeScript counts the claw on
    // Game.GetPlayer(), which inside a guest's relayed press is the guest's
    // figure, and a figure carries no inventory (one owner per inventory,
    // ADR-063). The press's scope carries the guest's record in, and this
    // row's hook answers the count from it; appended so no earlier row's
    // place moves
    Entry {
        native: "ObjectReference.GetItemCount",
        class: Class::Decided { cites: OWN_INVENTORY },
        live: true,
        note: &[
            "local: the seat's own read, answered inside another player's acting scope from that player's record (#117)",
        ],
        cells: &[Cell::Hook(Hook::ActingInventory)],
    },
];
