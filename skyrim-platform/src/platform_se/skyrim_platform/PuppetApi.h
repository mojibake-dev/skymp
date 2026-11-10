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
// setPuppetLocomotion(actorFormId, speed, directionDeg, turnRateDeg): the
// host's latest movement sample, which the Move detour writes every frame
// where the graph's Speed, Direction and TurnDelta channels read
// (docs/research/m3/re-puppet-locomotion.md)
Napi::Value SetPuppetLocomotion(const Napi::CallbackInfo& info);
// getHighProcessActors(): the engine's high-process actors by form id, the
// game thread's latest snapshot (Puppets.h)
Napi::Value GetHighProcessActors(const Napi::CallbackInfo& info);
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
// read it, as they read a script call's mark. The JS thread alone, no game
// call. False, nothing stored, for a quest 0, a stage outside -2 to 65535, a
// cause other than "world" or "seat", a root past QuestCause::kMaxRootBytes
// or outside [A-Za-z0-9_.], or a ttl outside 1 to QuestCause::kMaxMarkMs
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
// topicInfoFormId) makes a puppet say the host's line through the engine's
// own say (Offsets::Voice::SayTopic with the info given, so the engine's
// pick is skipped), queued to the game thread; sayTopicInfoResult answers
// -1 while it waits, 0 for no actor, info or process, 1 when the line
// started (the actor's sound callback state read 3 after the call), 2 when
// the engine refused it (a line still ending, the process's dialogue state),
// 3 when a line plays without the state byte (a live sound handle or a
// running timer: the sound descriptor path), 4 when the engine deferred it
// for a replay its AI update would run (a puppet's never comes)
Napi::Value SayTopicInfo(const Napi::CallbackInfo& info);
Napi::Value SayTopicInfoResult(const Napi::CallbackInfo& info);
// thuum docs/verbs/npc-stream.md (slice 5, the gaze and the face):
// pollHostedGaze(actorFormIds) reads, on the game thread, each actor's
// highest set head-track slot (HighProcessData::headTrackTarget and
// headTracked, CommonLibSSE-NG HighProcessData.h:217-218, the types at
// :80-94) and its emotion (Actor::GetEmotionType and GetEmotionValue,
// Actor.h:464-466) into a table; hostedGaze(actorFormId) answers the last
// read as [targetFormId, slot, emotionType, emotionValue], or null
Napi::Value PollHostedGaze(const Napi::CallbackInfo& info);
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

inline void Register(Napi::Env env, Napi::Object& exports)
{
  exports.Set("getHighProcessActors",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(GetHighProcessActors)));
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
    "pollNodeRotation",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(PollNodeRotation)));
  exports.Set(
    "nodeRotation",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(NodeRotation)));
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
}
}
