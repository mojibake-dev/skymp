scriptName TESModPlatform

function MoveRefrToPosition(ObjectReference refr, cell cell, WorldSpace world, Float posX, Float posY, Float posZ, Float rotX, Float rotY, Float rotZ) global native

Int function Add(Int a1, Int a2, Int a3, Int a4, Int a5, Int a6, Int a7, Int a8, Int a9, Int a10, Int a11, Int a12) global native

function SetWeaponDrawnMode(Actor actor, Int mode) global native

Int function GetNthVtableElement(Form pointer, Int pointerOffset, Int elementIndex) global native

Bool Function IsPlayerRunningEnabled() global native

ColorForm Function GetSkinColor(ActorBase base) global native

ActorBase Function CreateNpc() global native

ActorBase Function EvaluateLeveledNpc(String commaSeparatedListOfIds) global native

Function SetNpcSex(ActorBase npc, Int sex) global native

Function SetNpcRace(ActorBase npc, Race race) global native

Function SetNpcSkinColor(ActorBase npc, Int skinColor) global native

Function SetNpcHairColor(ActorBase npc, Int hairColor) global native

Function ResizeHeadpartsArray(ActorBase npc, Int newSize) global native

Function ResizeTintsArray(Int newSize) global native

Function SetFormIdUnsafe(Form form, Int newId) global native

Function ClearTintMasks(Actor targetActor) global native

Function PushTintMask(Actor targetActor, Int type, Int argb, String texturePath) global native

Function PushWornState(Bool worn, Bool wornLeft) global native

Function AddItemEx(ObjectReference containerRefr, Form item, Int countDelta, Float health, Enchantment enchantment, Int maxCharge, Bool removeEnchantmentOnUnequip, Float chargePercent, String textDisplayData, Int soul, Potion poison, Int poisonCount) global native

Function UpdateEquipment(Actor actor, Form item, Bool leftHand) global native

Function ResetContainer(Form container) global native

Function BlockPapyrusEvents(Bool block) global native

ObjectReference Function CreateReferenceAtLocation(Form baseForm, Cell cell, WorldSpace world, Float posX, Float posY, Float posZ, Float rotX, Float rotY, Float rotZ, Bool persist) global native

Function CloseMenu(string name) global native

; ADR-059 decision 1 (thuum docs/verbs/traps.md): the cells this seat hosts, so the
; Papyrus event block admits a host-scoped script's events here alone (the client's
; HostService calls it at every grant and release; the platform keeps the set).
Function SetCellHosted(Int cellFormId, Bool hosted) global native

Function SetGameDaysPassed(Float daysPassed) global native

; thuum docs/verbs/favorites.md: the player's favorites as (form id, hotkey) pairs,
; hotkey -1 for none; and marking one with a hotkey (-1 for none)
Int[] Function GetFavorites() global native

Bool Function SetFavorite(Form form, Int hotkey) global native

; thuum docs/verbs/actor-values.md: the player's 164 base values by actor value;
; one set; and its progress as xp, threshold, level, per skill level, xp and
; threshold, per skill its legendary count, read whole and written one skill,
; then the player's own experience and level, at a time (callNative takes no
; array argument)
Float[] Function GetActorValueBases() global native

Bool Function SetActorValueBase(Int av, Float base) global native

Float[] Function GetPlayerProgress() global native

Bool Function SetPlayerSkill(Int skill, Float level, Float xp, Float threshold, Int legendary) global native

Bool Function SetPlayerExperience(Float xp, Float threshold, Int level) global native

; thuum docs/verbs/magic-effects.md: while blocked, the player's own game
; applies no effect of a potion, a food or a poison to the player (the
; server's effects stand alone); and a server buff's change on one of the
; player's actor values, as ModActorValue makes it
Function SetPotionEffectsBlocked(Bool blocked) global native

Bool Function ModPlayerActorValue(Int av, Float delta) global native

; thuum docs/verbs/magic-effects.md, the hit art: an effect's hit effect art
; played on refr as the engine plays one (TESObjectREFR::ApplyArtObject), for
; duration seconds, -1 until stopped, a second play of the art on refr
; replacing this native's first; and the arts this native played on refr
; stopped, never one an engine effect plays there; false when nothing was
; played, or nothing stopped
Bool Function ApplyArtObject(ObjectReference refr, Art art, Float duration) global native
Bool Function StopArtObject(ObjectReference refr, Art art) global native

; thuum docs/verbs/stagger.md: the engine's own stagger of target at
; magnitude from source, as a hit's reaction starts it (the server's verdict)
Bool Function Stagger(Actor target, Float magnitude, Actor source) global native

; thuum docs/verbs/quest-stage.md H-56a: the Story Manager's Change Location
; event for actor, from oldLocation (none allowed) to newLocation (none reads
; the actor's current location), queued as the engine's own arrival queues it;
; a seat logging in inside a location raises none by itself
Bool Function SendStoryChangeLocation(Actor actor, Location oldLocation, Location newLocation) global native

; thuum docs/verbs/marksman.md: a neighbour's arrow, launched from its figure
; along its shooter's aim (pitch and heading, radians) with its draw's power
Bool Function LaunchArrow(Actor shooter, Weapon weapon, Ammo ammo, Float power, Float aimAngle, Float aimHeading) global native
Bool Function LaunchArrowAt(Actor shooter, Weapon weapon, Ammo ammo, Float power, Float angleX, Float angleZ, Float originX, Float originY, Float originZ) global native

; thuum docs/verbs/quest-stage.md, slice two: a stage the server relays is
; applied here through the engine's SetStage without its Papyrus fragment
; (the talker's game ran it): one mark on the pair before the SetStage, which
; the VM's quest stage sink consumes at the stage's first event; Release is
; the client's safety 10 s after the apply, for a mark no event claimed;
; false for no quest or a stage outside 0 to 65535
Bool Function SuppressQuestStageFragment(Quest quest, Int stage) global native
Bool Function ReleaseQuestStageFragment(Quest quest, Int stage) global native

; thuum docs/verbs/change-form.md (ADR-065, system 1): the reference's change
; form serialized on demand by the engine's own SaveGame (a JSON string:
; formType, flags, version, bodyHex, fnv1a, remap rows; "" when a premise
; refused, the reason in skyrim-platform.log), and a served record applied to
; this seat's loaded copy through the engine's own LoadGame and finish pass (a
; JSON string: applied, reason, fnvPatched). remapRows is
; "offset:formId:index,..." since callNative takes no array; flags is the
; sender's u32 as an Int
String Function SerializeChangeForm(ObjectReference refr) global native
String Function ApplyChangeForm(ObjectReference refr, String bodyHex, Int flags, Int version, String remapRows) global native
; the queued apply's answer (the apply runs as an SKSE task on the main thread
; since playtest 44): the JSON object above once, "" while the task has not run
String Function ApplyChangeFormResult(ObjectReference refr) global native

; thuum ADR-068 rung 3 (the script record, amendment A;
; docs/research/rung3-script-record-2026-11-06.md 1(a)): the reference's
; attached scripts read as the record carries them (a JSON string: formId,
; quiescent, fnv1a, seatLocal, scripts; or quiescent false and busy; "" when
; refused, the reason in skyrim-platform.log); a served record's scripts
; written over this seat's copies (a JSON string: applied, retry, reason,
; scripts, resolved, none, unresolved, seatLocal, stacksOnObject, holdUs,
; blanked, fnvAfter); and OnCellAttach, OnLoad or OnCellLoad sent to the
; reference's scripts and relayed as the engine's sinks raise it, with
; unseenOnly not when the engine served it since the last load (a JSON
; string: sent, seen, scripts, reason). scriptsJson is the record's scripts
; as a JSON array, since callNative takes no array
String Function SerializeScriptState(ObjectReference refr) global native
String Function ApplyScriptState(ObjectReference refr, String scriptsJson) global native
String Function SendScriptEvent(ObjectReference refr, String name, Bool unseenOnly) global native

; thuum ADR-068 D-C2 A (docs/research/fifty-fixes-2026-11-07.md 1(b)): a
; relayed trigger entry, OnTriggerEnter or OnTriggerLeave sent to the
; trigger's scripts and relayed to its aliases as the engine's trigger sinks
; raise it, with actionRef as the event's akActionRef (a JSON string: formId,
; event, sent, scripts, reason); refused for a name that is no trigger event,
; a None actionRef, an actor trigger and a reference with no attached script
String Function SendTriggerEvent(ObjectReference trigger, String name, ObjectReference actionRef) global native

; thuum ADR-068 D-W3 B (Eli 2026-11-10): the presser rule's held entries (an
; NPC's enter to a world trigger blanked at this seat's own load, its owner
; undecided then) whose presser is this seat's now, with the trigger in a
; cell this seat hosts, each replayed once through SendTriggerEvent's body
; under the platform's mark; the expired ones dropped (a JSON string:
; replayed, refused, expired, held)
String Function ReplayHeldEntries() global native

; thuum ADR-068 rung 4 (the state rows, amendment B): the world globals this
; seat's engine changed since its last answer, for the world streamer's
; WorldGlobal 80 (a JSON string: [[formId, value], ...], at most 512
; entries, "[]" for none); constants, non-finite values and the
; comma-separated hex ids in exclude are never answered; with reset, every
; other global's value now becomes the baseline and the answer is "[]"
String Function DiffWorldGlobals(Bool reset, String exclude) global native

; thuum ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md 1(c) and
; 8(b); D-V1 B, D-T2 B): a quest's or a reference's script variables read by
; name for ScriptVariables 81 (a JSON string: an array, in the keys' order,
; of { form, script, var, k, i|f|b } with k 3 int, 4 float, 5 bool, or of
; { form, script, var, reason }; "[]" for keys that are no JSON array), and
; written by name through Variable's setters, a kind other than the
; variable's declared one refused (a JSON string: per entry { form, script,
; var, written, reason }). A key is { form, script, var }, names without
; case; var "<OnUpdateGameTime>" reads the form's queued game-time
; registration's hours left and "<OnUpdateGameTime:repeat>" a repeat's
; interval, never written. keysJson and entriesJson are JSON arrays, since
; callNative takes no array
String Function ReadScriptVariables(String keysJson) global native
String Function WriteScriptVariables(String entriesJson) global native
