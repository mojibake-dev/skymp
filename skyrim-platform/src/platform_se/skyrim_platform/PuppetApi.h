#pragma once

#include "NapiHelper.h"

namespace PuppetApi {

// thuum docs/verbs/npc-puppet.md: setPuppet(actorFormId, puppet). true: the
// demote, re-ai-puppet.md Q4 (combat and alarm stopped, the keep-offset
// cleared, the AI bit cleared, the actor flagged for the detours); false:
// the promote (unflagged, the AI bit set, the package evaluated). The flag
// flips at once; the engine calls run on the game thread right after this
// update (performAction's shape). Returns false when no such actor exists.
Napi::Value SetPuppet(const Napi::CallbackInfo& info);
// isPuppet(actorFormId): whether this game flagged the actor
Napi::Value IsPuppet(const Napi::CallbackInfo& info);
// releasePuppet(actorFormId): a puppet this engine no longer loads is left
// to its own schedule (thuum docs/verbs/npc-handoff.md D11): the flag off at
// once, then on the game thread the activation unblocked and the AI bit set,
// no package evaluation (an unloaded actor has no process). False for an
// invalid id. The demote poll makes it a puppet again within pollMs when it
// loads on a seat that does not host it; the host's roster sights it.
Napi::Value ReleasePuppet(const Napi::CallbackInfo& info);
// setPuppetLocomotion(actorFormId, speed, directionDeg, turnRateDeg,
// headingDeg): the host's latest movement sample, which the Move detour
// writes every frame where the graph's Speed, Direction and TurnDelta
// channels read, turning the puppet toward headingDeg at turnRateDeg
// (docs/research/m3/re-puppet-locomotion.md)
Napi::Value SetPuppetLocomotion(const Napi::CallbackInfo& info);
// getActorLocomotion(actorFormId): tracker #111, the other end of
// setPuppetLocomotion's fields: an actor this game simulates, its engine's
// own speed, move direction and turn rate as the movement step left them
// (Puppets.h), {speed, directionDeg, turnRateDeg, ageMs} or null
Napi::Value GetActorLocomotion(const Napi::CallbackInfo& info);
// setPuppetDetection(actorFormId, level): thuum docs/verbs/guest-witness.md,
// a puppet's detection of this game's player as its host relayed it, which
// RequestDetectionLevel answers from then on; null forgets it
Napi::Value SetPuppetDetection(const Napi::CallbackInfo& info);
// getDetectionLevels(detectorFormIds, targetFormIds): each detector's level of
// each target as this game's engine holds it, row by detector
Napi::Value GetDetectionLevels(const Napi::CallbackInfo& info);
// getHighProcessActors(): the engine's high-process actors by form id, the
// game thread's latest snapshot (Puppets.h)
Napi::Value GetHighProcessActors(const Napi::CallbackInfo& info);
// getPackageDestinationCells(actorFormId): the cells the actor's running
// package names as its location, the game thread's latest snapshot
// (Puppets.h; tracker #121)
Napi::Value GetPackageDestinationCells(const Napi::CallbackInfo& info);
// thuum docs/verbs/npc-stream.md (ADR-050): setHosted(actorFormId, hosted),
// the server's word that this game simulates the actor (true) or not
// (false), kept in the hosted set the hook on the engine's action entry
// reads (Hosted.h); the JS thread alone, no game call. False for 0 and for
// the player's own id (its actions are reported regardless).
Napi::Value SetHosted(const Napi::CallbackInfo& info);
// isHosted(actorFormId): whether the set holds the actor
Napi::Value IsHosted(const Napi::CallbackInfo& info);
// thuum docs/DECISIONS.md ADR-068 rung 5 (the acting scope, ActingScope.h):
// setActingPlayer(questId, stage, figureLocalId, serverId, ttlMs), the acting
// player of a relayed stage (0 to 65535), start (-1), stop (-2) or trigger
// entry (-3, questId the trigger), written on the JS thread with no game
// call before the client applies it; a start or a stop arms the quest's
// scope at once. False for a quest 0, a stage outside -3 to 65535, a
// serverId 0, the player's own id as the figure, a ttl outside 1 to 60000
Napi::Value SetActingPlayer(const Napi::CallbackInfo& info);
// a player's own game's look at a target as the server relayed it
// (PlayerLook): the player by its server id (0xff000000 and up), the target
// a load-order form, whether the player sees it; kept in the look table the
// seam answers Actor.HasLOS on that player's figure from (ActingScope::Looks).
// False, nothing stored, for a player below 0xff000000 or a target of 0 or
// 0xff000000 and up; no game call
Napi::Value SetPlayerLook(const Napi::CallbackInfo& info);
// train 53's V-scene (ADR-073 D-A2 C): setRelayedScope(questId, stage,
// ttlMs), a relayed stage (0 to 65535), start (-1) or stop (-2) whose cause
// seat is elsewhere and whose acting player is this seat's own or unnamed:
// requested, and a word armed at once, rooted kRelayed with no acting player
// (ActingScope::Swaps false), so a scene start its fragment makes is skipped
// (the cause seat starts it and claims its actors, ADR-073 step 5a) and
// nothing is swapped. False for a quest 0, a stage outside -2 to 65535, a ttl
// outside 1 to 60000
Napi::Value SetRelayedScope(const Napi::CallbackInfo& info);
// getActingPlayer(questId, stage): the server id of the acting player whose
// scope requested the pair here (relayed, or chained by a scoped stack), 0
// for none; the C2S QuestStage's `acting`
Napi::Value GetActingPlayer(const Napi::CallbackInfo& info);
// thuum docs/DECISIONS.md ADR-068 rung 5, D-Q4 C (QuestCause.h):
// markQuestCause(questId, stage, cause, root, ttlMs), a cause this seat's own
// act names for the quest's next start (-1), stop (-2) or stage (0 to 65535)
// before the engine makes it, for ttlMs: skymp5-client's console service
// marks a quest command the player typed. QuestCause::Mark's table, so the
// word's event takes it once and the stage's event and the quest stage sink
// read it, as they read a script call's mark. Since train 52 (Fix 3) the word
// marked is QuestCause::WordMade's for the quest's run and Start Game
// Enabled flag, read here (a form-table read and two flag reads on the JS
// thread, no Papyrus call): a stage itself, a start only for a quest not
// running, a stop only for a running one, and a typed reset (-3) the word it
// makes. Answers { marked, running, startsEnabled }, marked the word stored
// (a stage, -1 or -2) or null when the request makes none; null, nothing
// stored, for a quest 0, a stage outside -3 to 65535, a cause other than
// "world" or "seat", a root past QuestCause::kMaxRootBytes or outside
// [A-Za-z0-9_.], a ttl outside 1 to QuestCause::kMaxMarkMs, or a quest id no
// TESQuest has
Napi::Value MarkQuestCause(const Napi::CallbackInfo& info);
// setPlayerGlobals(globalFormIds): PQ-3's set, the globals each player keeps
// (the PlayerGlobals watch list), replaced whole; a scoped stack's
// GlobalVariable.SetValue on one is skipped. The JS thread alone, no game
// call; true when stored
Napi::Value SetPlayerGlobals(const Napi::CallbackInfo& info);
// setPlayerQuests(questFormIds): the per-player quests (the server's derived
// playerQuests), replaced whole; the Papyrus event gate passes an event to
// such a quest's own scripts and its aliases' scripts on every seat, for its
// own player. The JS thread alone, no game call; true when stored
Napi::Value SetPlayerQuests(const Napi::CallbackInfo& info);
// isPerformingAction(actorFormId): whether the engine's action entry is open
// on the actor right now (Hosted::IsPerforming, the hook's mark); read
// inside a sendAnimationEvent hook handler, where the game thread waits, it
// says the event being sent is the action's own
Napi::Value IsPerformingAction(const Napi::CallbackInfo& info);
// thuum docs/DECISIONS.md ADR-068 D-S3 (traps follow the interaction;
// TrapFamily.h): getTrapRole(formId) answers { family, playerOnly, destroyed,
// trigger, exempt } for a reference with an attached script, from the gate's
// own reads on the JS thread (the VM's attached scripts under its lock, the
// reference's destroyed flag, its trigger's playerOnlyTrigger property,
// whether a script on it reaches TrapTriggerBase: a trap's trigger, never a
// trap body, playtest 48; and since ADR-068 D-W2 A whether a script on it is
// of a class setTriggerPresserExempt names, TrapFamily::Exempt, so the
// client skips its root report for a trigger every seat fires itself), and
// null for an id that names no reference or one with no attached script
Napi::Value GetTrapRole(const Napi::CallbackInfo& info);
// thuum docs/DECISIONS.md ADR-068 D-W1 A and D-C2 A (the presser rule,
// FridaHooks.h): setTriggerPresserRule(on), the Papyrus event gate's rule over
// a world object's OnTriggerEnter and OnTriggerLeave on or off, off by
// default (D-S3's every-seat firing): on, an entry whose presser this seat
// does not own is blanked, and an owned one on a trap family trigger, or on
// a scripted trigger in a cell this seat does not host, raises triggerEntry
// on the next tick. Stored with no game call; true when stored
Napi::Value SetTriggerPresserRule(const Napi::CallbackInfo& info);
// thuum docs/research/fifty-fixes-2026-11-07.md 1(b) (the furniture branch,
// FridaHooks.h): setFurnitureAnimation(on), the animation hook's rule over a
// furniture's own graph event on or off, off by default: on, an event the
// engine sends to a plain reference whose base is a FURN never reaches the
// sendAnimationEvent handlers and raises furnitureAnimation on the next tick.
// Stored with no game call; true when stored
Napi::Value SetFurnitureAnimation(const Napi::CallbackInfo& info);
// thuum docs/DECISIONS.md ADR-068 D-W2 A (ScriptNatives.h, TrapFamily.h):
// setTriggerLook(on), the look's switch, off by default, acting only with the
// presser rule on: on, a trap family trigger's own call on itself in its
// firing window is queued in scriptCalls as kind "look" and still runs here;
// setTriggerPresserExempt(classes), the fallback lever, the script classes
// whose family references keep D-S3 under the presser rule (a non-owned
// press passes, an owned one opens no look), replaced whole, empty by
// default. Both stored with no game call; true when stored
Napi::Value SetTriggerLook(const Napi::CallbackInfo& info);
Napi::Value SetTriggerPresserExempt(const Napi::CallbackInfo& info);
// setPropFollowed(formId, followed): a trap body another seat streams (true)
// or no longer (false), kept in the set the native seam reads, where this
// seat's script SetMotionType and ApplyHavokImpulse on the body run
// neutralized; setRecordApplying(formId, applying): a reference whose server
// record skymp5-client is applying, every Papyrus event to it blanked
// meanwhile (the gate's blank precedes the sendPapyrusEvent hook, so JS
// hears none of them either). Both store on the JS thread alone, no game
// call: true when stored, false for 0 and for the player's own id
Napi::Value SetPropFollowed(const Napi::CallbackInfo& info);
Napi::Value SetRecordApplying(const Napi::CallbackInfo& info);
// thuum docs/DECISIONS.md ADR-071 rung 2 (the sample v2; a prop's place is
// its body's, never its reference's: docs/engine/havok-props.md E21):
// readBodies(formIds, all) reads each loaded reference's Havok body on the JS
// thread under no lock (PuppetApi.cpp's ReadBody says why), as
// { formId, awake, motion, pos, rot, lin, ang }: pos the 3D root's world
// translate in game units, rot its world rotation as Euler XYZ degrees, lin
// in game units a second, ang in radians a second (Havok's own), motion
// hkpMotion::MotionType's value, awake the body's island in the world's
// active list. Since rung 4 the bodies are PropBodies' walk of the whole 3D
// (PropBodies.h): a whole reference (one body, not Fixed) answers its one
// body, found at any depth, and one with no part its first body (an
// all-Fixed static, motion 5); a reference with a part (a body that is not
// Invalid, Fixed or Character) answers awake when any part is, motion, lin
// and ang the fastest part's, and bones, every part's pose in walk order as
// { pos: [x, y, z] in game units, quat: [x, y, z, w] }, none when the walk
// was cut short; an id with no reference, 3D, cell world or rigid body is
// left out, as is one whose walk faulted (said in the log). pos and rot stay
// the root's either way. With all false a body is answered while awake and
// once more at the read where it fell asleep (rule 9: a resting body costs JS
// nothing); with all true every body found, that filter left as it was
Napi::Value ReadBodies(const Napi::CallbackInfo& info);
// setBodyVelocity(formId, lin, ang): the take-over's hand-off, the body's
// linear (game units a second) and angular (radians a second) velocity set
// through CommonLib's bhkRigidBody setters on the main thread, queued on
// SKSE's UI task queue and never waited for. True when queued; false for 0,
// the player's own id, a non-finite component or no SKSE task interface. The
// task logs the set, or why not (no 3D, no rigid body, since rung 4 a
// reference of several bodies, which imposeBodies drives, a motion type that
// is not dynamic)
Napi::Value SetBodyVelocity(const Napi::CallbackInfo& info);
// thuum docs/DECISIONS.md ADR-071 decision 4 (rung 3, FridaHooks.h):
// setImpulseCauseRule(on), the impulse rule at the replaced
// ScaleGameplayImpulseForce and AddExplosionImpulse on or off, off by default
// (every impulse the engine's own): on, an impulse whose cause another seat
// owns is zeroed on every physics body, and one this seat's cause gave
// raises propImpulse on the next tick. Stored with no game call; true when
// ScaleGameplayImpulseForce's replacement went in at install. And rung 3's
// third argument to readBodies, islands (the island's claim): with islands
// true a body of a dynamic motion type that is awake carries island, its
// island's other members of a dynamic type as { refrId, formType }, each
// reference once with its base's form type, at most 16, never the player's
// own or the body's own reference, none for the world's fixed island, read
// on the JS thread with no engine lock (absent when the read faulted);
// islands absent or false reads none, rung 2's call
Napi::Value SetImpulseCauseRule(const Napi::CallbackInfo& info);
// thuum docs/DECISIONS.md ADR-071 rung 4 (statics and a body's parts; D-PH9
// A, D-PH10 A, the route K3 under D-PH26 A): getBodyClass(formId), the class
// each seat reads from its loaded 3D, { bsx, dynamic, bodies, fixed, parts:
// [{ node, motion }], cut }: bsx the 3D root's BSX flags (0 with no block),
// dynamic their kDynamic bit, then PropBodies' walk (bodies every rigid body,
// fixed the Fixed ones, parts each part's node name, "" for none, and motion
// type in walk order, cut a walk past 64 parts, 512 nodes or 32 levels); null
// with no reference, no 3D or a faulted walk. On the JS thread, no task, no
// log. imposeBodies(formId, bones, rest), the follower's newest poses for the
// reference's parts in walk order ({ pos in game units, quat [x, y, z, w] }),
// stored on the JS thread and laid on the parts by one drain on the main
// thread: rest, or a part more than 64 units off, places every part
// outright (a rest then drops the form), else every part is driven by
// velocity toward its pose over the world's time since the form's last
// drain (ImposeHorizonOf, thuum tracker #125); true when stored, false for
// 0, the player's own id, no pose or more than 64, a value not a number, a
// quaternion off unit length by more than 0.01, no SKSE task interface or a
// full table. releaseBodies(formId), the form's stored poses dropped (a
// take-over, a quiet stream, a follow's end), true when poses were held.
// setWorldBodyContacts(on), the contact hook's world-body switch, off by
// default (rung 3's filter, layers 4 and 10 alone): on, a body of a dynamic
// motion type on any layer reaches propContact too; stored with no game
// call, true
Napi::Value GetBodyClass(const Napi::CallbackInfo& info);
Napi::Value ImposeBodies(const Napi::CallbackInfo& info);
Napi::Value ReleaseBodies(const Napi::CallbackInfo& info);
Napi::Value SetWorldBodyContacts(const Napi::CallbackInfo& info);
// thuum docs/DECISIONS.md ADR-068 rung 3 (the script record, amendment A;
// ScriptState.h): setScriptRecordPending(formId, pending), a reference whose
// served script record waits for its apply (true) or no longer (false), kept
// in the set the Papyrus event gate reads, where its OnLoad and OnCellLoad
// are blanked meanwhile and their names kept for ApplyScriptState's answer;
// TESModPlatform.ApplyScriptState lowers it when it applies the record.
// Stored on the JS thread alone, no game call: true when stored, false for 0
// and for the player's own id
Napi::Value SetScriptRecordPending(const Napi::CallbackInfo& info);
// releaseScriptRecordPending(formId): the mark lowered, answering the JSON
// array of the event names the gate blanked while it was up ("[]" for none,
// for 0 and for the player's own id), so a client that gives a record up can
// still send them through SendScriptEvent (setScriptRecordPending(id, false)
// drops them); a table operation on the JS thread alone, no game call
Napi::Value ReleaseScriptRecordPending(const Napi::CallbackInfo& info);
// setScriptRecordTouches(enabled): the seam's switch on its touch (an owned
// state row's call batched as kind "touch", ScriptNatives.h), one atomic
// flag, on by default; with false no such call is read for its caller or
// queued. No game call: true when stored
Napi::Value SetScriptRecordTouches(const Napi::CallbackInfo& info);
// thuum docs/verbs/npc-stream.md (the voice slice): sayTopicInfo(actorFormId,
// topicInfoFormId, topicFormId?) makes a puppet say the host's line through
// the engine's own say (Offsets::Voice::SayTopic with the info given, so the
// engine's pick is skipped), queued to the game thread; sayTopicInfoResult
// answers -1 while it waits, 0 for no actor or process, 1 when the line
// started (the actor's sound callback state read 3 after the call), 2 when
// the engine refused it (a line still ending, the process's dialogue state),
// 3 when a line plays without the state byte (a live sound handle or a
// running timer: the sound descriptor path), 4 when the engine deferred it
// for a replay its AI update would run (a puppet's never comes), 5 when the
// engine judged it inaudible here. Tracker #123: an info this game does not
// resolve is looked for through the relayed topic (its info array, then the
// engine's pick with a null info); 6 when no line resolves here, 7 when the
// engine's pick took another info of the topic (stopped and cleared). A
// topic listing no info whose quest is stopped first has that quest's
// dialogue loaded on the main thread (Offsets::Voice::LoadQuestDialogue;
// the load waits, bounded, while any quest's promote task is in flight, and
// one that left the topic empty is not tried again), the answer -1 until
// the say posted back to gameThrQ writes it
Napi::Value SayTopicInfo(const Napi::CallbackInfo& info);
Napi::Value SayTopicInfoResult(const Napi::CallbackInfo& info);
// thuum docs/verbs/npc-stream.md (slice 5, the gaze and the face):
// pollHostedGaze(actorFormIds) reads, on the game thread, each actor's
// highest set head-track slot (HighProcessData::headTrackTarget and
// headTracked, CommonLibSSE-NG HighProcessData.h:217-218, the types at
// :80-94) and its emotion (Actor::GetEmotionType and GetEmotionValue,
// Actor.h:464-466) into a table; hostedGaze(actorFormId) answers the last
// read as [targetFormId, slot, emotionType, emotionValue], or null. The same
// read takes the actor's running effects whose look the engine keeps
// (thuum docs/verbs/npc-puppet.md, Ownership: ReadEffectLooks in
// PuppetApi.cpp); hostedEffects(actorFormId) answers them as base effect
// form ids, ascending, each once, or null when the last poll did not read
// the actor. Each of the three takes an optional own flag: true is the
// seat's own poll, whose table its sends read; absent or false is any other
// reader's (the lab's), a table of its own, so a lab read never replaces the
// seat's (tracker #56)
Napi::Value PollHostedGaze(const Napi::CallbackInfo& info);
Napi::Value HostedEffects(const Napi::CallbackInfo& info);
// thuum tracker #123 (playtest 55, the spoken-line read):
// spokenLines(actorFormId, sinceMs?) answers the speaker's rows from the
// line store (FridaHooks.h SpokenLine; recorded at the engine's
// SendTopicInfoEvent before the fragment gate decides, and at SayTopicInfo's
// own cuts), oldest first, as [epochMs, infoId, topicId, type (0 BEGIN,
// 1 END, 2 CUT), gated, handleValid, responseIndex, voiceTimerSec], only
// rows with epochMs >= sinceMs; [] when none. A table read under its mutex,
// no game call: safe from any JavaScript context
Napi::Value SpokenLines(const Napi::CallbackInfo& info);
// thuum docs/verbs/npc-talk.md (ADR-052): activatePickRef() runs the
// player's own activation of its crosshair pick on the game thread
// (PlayerCharacter::ActivatePickRef, CommonLibSSE-NG PlayerCharacter.h:544),
// the E the claim's word made meaningful; true when queued
Napi::Value ActivatePickRef(const Napi::CallbackInfo& info);
Napi::Value HostedGaze(const Napi::CallbackInfo& info);
// pollNodeRotation(refrId, nodeNames) reads, on the game thread, the world
// and local rotation (Euler XYZ, radians) of each named node of a loaded
// reference's 3D ("" for the root); nodeRotation(refrId) answers the last
// poll as [{node, has, world: [x, y, z], local: [x, y, z]}], null before one
// (docs/verbs/traps.md "The pillar's side": the lab's view read of a turned
// puzzle pillar, which no Papyrus native can give without an out-array)
Napi::Value PollNodeRotation(const Napi::CallbackInfo& info);
Napi::Value NodeRotation(const Napi::CallbackInfo& info);
// thuum D-53-8 B (train 53's V-live): pollOwnHeadParts() reads, on the game
// thread, the head parts the player's own base draws: its overlay list
// (CommonLibSSE-NG TESNPC::GetBaseOverlays and GetNumBaseOverlays) while
// TESNPC::HasOverlays answers true and the list holds parts, else its base
// list (TESNPC::headParts, numHeadParts), with the base's race and whether
// that race carries a head part list flag; ownHeadParts() answers the last
// read as { seq, race, flagged, hasOverlays, overlayCount, overlay, parts },
// or null before one (OwnLookService reports it to the server after the
// player's own race changed, OwnHeadParts 82)
Napi::Value PollOwnHeadParts(const Napi::CallbackInfo& info);
Napi::Value OwnHeadParts(const Napi::CallbackInfo& info);
// thuum playtest 37 (the probe before any head build): pollFaceKeyframes
// (actorFormId) copies, on the game thread, the actor's FaceGen keyframes
// (BSFaceGenAnimationData's expression, modifier and phoneme keyframes,
// CommonLibSSE-NG BSFaceGenAnimationData.h:39-42) and its override flag;
// faceKeyframes(actorFormId) answers the last read as [expression[],
// modifier[], phoneme[], override, sets[][]], or null; sets are every
// keyframe set of BSFaceGenAnimationData, 0x20 to 0x180 in header order
// (2026-10-28: the lip sync's set is among the unnamed ones)
Napi::Value PollFaceKeyframes(const Napi::CallbackInfo& info);
Napi::Value FaceKeyframes(const Napi::CallbackInfo& info);
// thuum memo D52 B (2026-10-28): refreshContainerMenu(refId, mode) tells an
// open ContainerMenu on that reference to redraw: mode bit 1 the engine's
// inventory-update message, bit 2 the item list's re-read, 3 both (the
// default); true when queued
Napi::Value RefreshContainerMenu(const Napi::CallbackInfo& info);
// thuum docs/research/m3/re-claimed-dialogue-state.md (2026-10-28):
// consumeActivatePress() marks the current E press's held action done, so
// the engine's own release activates nothing (one activation per claim)
Napi::Value ConsumeActivatePress(const Napi::CallbackInfo& info);
// thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
// damageActor(actorFormId, health, attackerFormId, killMove): the server's
// cause applied to an actor this game simulates on the game thread, through
// the engine's own damage entry: the delta from the engine's current health
// to the resulting percentage, downward through DamageActor with the
// attacker (the engine reacts, records the killer, dies by itself at zero),
// upward through the value owner's restore; false for an invalid id or a
// health that is not a number
Napi::Value DamageActor(const Napi::CallbackInfo& info);
// setEngineDamageCut(on): the replacement's cut (FridaHooks.cpp), the client
// setting npcEngineDamage's mirror
Napi::Value SetEngineDamageCut(const Napi::CallbackInfo& info);
// isDamageActorReplaced(): whether DamageActor's replacement went in, the
// player's cut of another actor's damage with it (thuum D-T4); the client
// keeps its world multiplier at 0 without it
Napi::Value IsDamageActorReplaced(const Napi::CallbackInfo& info);
// setPlayerDifficulty(level): the server's difficulty, 0 (Novice) to 5
// (Legendary), written on the game thread to the player's own difficulty
// field (CommonLibSSE-NG include/RE/P/PlayerCharacter.h:399,
// GetGameStatsData's difficulty), which DamageActor (37335) indexes for
// every hit this engine applies; written and logged only when it differs.
// False for a level outside 0 to 5
Napi::Value SetPlayerDifficulty(const Napi::CallbackInfo& info);
// thuum #63: pollLinkedDoors(refrIds) reads, on the game thread (one task per
// call; a lookup on the JS thread can wait on a cell load's form map lock,
// SetPuppet's note), each named reference's load door link (CommonLibSSE-NG
// include/RE/E/ExtraTeleport.h:12, DoorTeleportData::linkedDoor, under the
// extra list's own lock, BSExtraDataList::GetByType); linkedDoorOf(refrId)
// answers the last read: the other side's form id, 0 for a reference that
// is no load door, null before any read of it. A door's link is set at its
// load and never moves in play
Napi::Value PollLinkedDoors(const Napi::CallbackInfo& info);
Napi::Value LinkedDoorOf(const Napi::CallbackInfo& info);
// thuum #63 and #47: pollCellGrids(cellIds) reads, on the game thread (one
// task per call), each named exterior cell's grid square (CommonLibSSE-NG
// include/RE/T/TESObjectCELL.h:58-59, EXTERIOR_DATA's cellX and cellY,
// through GetCoordinates) and its worldspace; cellGridOf(cellId) answers the
// last read: {world, x, y} for an exterior cell, {world: 0} for an interior,
// null before any read. A persistent exterior reference reports its
// worldspace's persistent cell as its parent, so the client finds the cell a
// door stands in by its position against these squares
Napi::Value PollCellGrids(const Napi::CallbackInfo& info);
Napi::Value CellGridOf(const Napi::CallbackInfo& info);

inline void Register(Napi::Env env, Napi::Object& exports)
{
  exports.Set("getHighProcessActors",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(GetHighProcessActors)));
  exports.Set(
    "getPackageDestinationCells",
    Napi::Function::New(
      env, NapiHelper::WrapCppExceptions(GetPackageDestinationCells)));
  exports.Set(
    "setPuppet",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetPuppet)));
  exports.Set(
    "isPuppet",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(IsPuppet)));
  exports.Set(
    "releasePuppet",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ReleasePuppet)));
  exports.Set("setPuppetLocomotion",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetPuppetLocomotion)));
  exports.Set("setPuppetDetection",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetPuppetDetection)));
  exports.Set("getDetectionLevels",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(GetDetectionLevels)));
  exports.Set("getActorLocomotion",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(GetActorLocomotion)));
  exports.Set(
    "setHosted",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetHosted)));
  exports.Set(
    "isHosted",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(IsHosted)));
  exports.Set(
    "setActingPlayer",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetActingPlayer)));
  exports.Set(
    "setPlayerLook",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetPlayerLook)));
  exports.Set(
    "setRelayedScope",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetRelayedScope)));
  exports.Set(
    "getActingPlayer",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(GetActingPlayer)));
  exports.Set(
    "markQuestCause",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(MarkQuestCause)));
  exports.Set(
    "setPlayerGlobals",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetPlayerGlobals)));
  exports.Set(
    "setPlayerQuests",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetPlayerQuests)));
  exports.Set("isPerformingAction",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(IsPerformingAction)));
  exports.Set(
    "getTrapRole",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(GetTrapRole)));
  exports.Set("setTriggerPresserRule",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetTriggerPresserRule)));
  exports.Set("setFurnitureAnimation",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetFurnitureAnimation)));
  exports.Set(
    "setTriggerLook",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetTriggerLook)));
  exports.Set("setTriggerPresserExempt",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetTriggerPresserExempt)));
  exports.Set(
    "setPropFollowed",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetPropFollowed)));
  exports.Set("setRecordApplying",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetRecordApplying)));
  exports.Set(
    "readBodies",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ReadBodies)));
  exports.Set(
    "setBodyVelocity",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetBodyVelocity)));
  exports.Set("setImpulseCauseRule",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetImpulseCauseRule)));
  exports.Set(
    "getBodyClass",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(GetBodyClass)));
  exports.Set(
    "imposeBodies",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ImposeBodies)));
  exports.Set(
    "releaseBodies",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ReleaseBodies)));
  exports.Set("setWorldBodyContacts",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetWorldBodyContacts)));
  exports.Set("setScriptRecordPending",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetScriptRecordPending)));
  exports.Set(
    "releaseScriptRecordPending",
    Napi::Function::New(
      env, NapiHelper::WrapCppExceptions(ReleaseScriptRecordPending)));
  exports.Set("setScriptRecordTouches",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetScriptRecordTouches)));
  exports.Set(
    "sayTopicInfo",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SayTopicInfo)));
  exports.Set("sayTopicInfoResult",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SayTopicInfoResult)));
  exports.Set(
    "activatePickRef",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ActivatePickRef)));
  exports.Set(
    "pollHostedGaze",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(PollHostedGaze)));
  exports.Set(
    "hostedGaze",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(HostedGaze)));
  exports.Set(
    "hostedEffects",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(HostedEffects)));
  exports.Set(
    "spokenLines",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SpokenLines)));
  exports.Set(
    "pollNodeRotation",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(PollNodeRotation)));
  exports.Set(
    "nodeRotation",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(NodeRotation)));
  exports.Set(
    "pollOwnHeadParts",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(PollOwnHeadParts)));
  exports.Set(
    "ownHeadParts",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(OwnHeadParts)));
  exports.Set("pollFaceKeyframes",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(PollFaceKeyframes)));
  exports.Set(
    "faceKeyframes",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(FaceKeyframes)));
  exports.Set("refreshContainerMenu",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(RefreshContainerMenu)));
  exports.Set("consumeActivatePress",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(ConsumeActivatePress)));
  exports.Set(
    "damageActor",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(DamageActor)));
  exports.Set("setEngineDamageCut",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetEngineDamageCut)));
  exports.Set("isDamageActorReplaced",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(IsDamageActorReplaced)));
  exports.Set("setPlayerDifficulty",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetPlayerDifficulty)));
  exports.Set(
    "pollLinkedDoors",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(PollLinkedDoors)));
  exports.Set(
    "linkedDoorOf",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(LinkedDoorOf)));
  exports.Set(
    "pollCellGrids",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(PollCellGrids)));
  exports.Set(
    "cellGridOf",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(CellGridOf)));
}
}
