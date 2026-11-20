#pragma once

#include <RE/B/BSCoreTypes.h>
#include <cstdint>
#include <string>
#include <vector>

namespace RE {
class Actor;
class DialogueItem;
}

namespace RE::BSScript {
struct IObjectHandlePolicy;
}

namespace Frida {

void InstallHooks();

// thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
// the engine's own damage entry (DamageActor, the replaced function's
// original), for the server's cause applied to an actor this game simulates:
// the engine reacts, records the killer and dies by itself at zero
bool DamageActorOriginal(RE::Actor* victim, float damage, RE::Actor* attacker,
                         bool killMove);
// the replacement's cut of this game's own engine damage to a puppet, and to
// an NPC this game simulates when the hand is a player's or a figure's (the
// server's number is the one number); default on, skymp5-client's
// npcEngineDamage switches it
void SetEngineDamageCut(bool on);
// whether DamageActor's replacement went in at install: the player's cut of
// another actor's damage rests on it (thuum D-T4)
bool IsDamageActorReplaced();

// thuum docs/DECISIONS.md ADR-059 decision 1 and ADR-062 decision 1: the
// cells this game hosts, so a Papyrus event to a world object standing in one
// of them passes the event block whatever its script's name (OnSendEventEnter)
// and a hooked native's call on such an object counts as the host's
// (ScriptNatives.cpp). skymp5-client's hostService tells it at every grant
// and release (TESModPlatform's SetCellHosted). Threadsafe.
void SetCellHosted(uint32_t cellFormId, bool hosted);
// whether this game hosts the cell (ADR-059 decision 1's set), for the hooks
// outside this file (ScriptNatives.cpp)
bool IsCellHosted(uint32_t cellFormId);

// thuum docs/DECISIONS.md ADR-068 D-W1 A and D-C2 A (the presser rule;
// docs/research/fifty-fixes-2026-11-07.md 1(b)): the Papyrus event gate's
// rule over a world object's OnTriggerEnter and OnTriggerLeave on or off
// (OnSendEventEnter). On, an entry whose presser this seat does not own is
// blanked, and one it owns on a trap family trigger, or on a scripted
// trigger in a cell it does not host, raises triggerEntry; off, the default,
// keeps D-S3 (every seat's engine counts every presser), so a client that
// never sets it changes nothing. skymp5-client's setting triggerPresser
// turns it on through setTriggerPresserRule (PuppetApi.cpp). Threadsafe
void SetTriggerPresserRule(bool on);
// the presser rule's switch as last set: ADR-068 D-W2 A's look acts only
// with it on (ScriptNatives.cpp OnFamilyCall)
bool TriggerPresserRuleOn();

// thuum docs/DECISIONS.md ADR-071 decision 4 (rung 3, an impulse's cause;
// docs/verbs/prop-physics.md): the impulse rule on or off at the replaced
// ScaleGameplayImpulseForce and AddExplosionImpulse (FridaHooks.cpp). On, an
// impulse whose cause another seat owns is zeroed (every physics body, Eli's
// D-PH5 "Every physics body"), and one whose cause this seat owns applies and
// raises propImpulse with the references it pushed; off, the default, every
// impulse applies as the engine's own and nothing is counted, so a client
// that never sets it changes nothing. skymp5-client's setting
// propImpulseCause turns it on through setImpulseCauseRule (PuppetApi.cpp).
// Stored either way; true when ScaleGameplayImpulseForce's replacement went
// in at install (the props' push can be zeroed). Threadsafe
bool SetImpulseCauseRule(bool on);

// thuum docs/DECISIONS.md ADR-071 rung 4 (D-PH9 A, a world body admitted by
// any touch; thuum .claude/rules/native.md's kill switch): the contact hook's
// world-body switch (OnPropContactEnter, FridaHooks.cpp). On, the player's
// contact with a body of a dynamic motion type reaches propContact on any
// collision layer (a world body's part on its own layer, kClutterLarge 29, a
// corpse's kDeadBip 32), beside rung 3's clutter and props layers at any
// motion; off, the default, keeps rung 3's filter, so a client that never
// sets it changes nothing. skymp5-client's setting propWorldBodies turns it on
// through setWorldBodyContacts (PuppetApi.cpp). Logs "[verb:prop-physics]
// world-body contacts on" or "off". Threadsafe
void SetWorldBodyContacts(bool on);

// thuum tracker #178 and #175 (the shout design's step 3; the re-analyst's
// static read of 2026-11-19): the get-up hold. A copy whose parts the drive
// imposes from another seat's stream (PuppetApi's imposeBodies) never gets up
// by its own engine's knock step while the stream lives: the get-up
// (Address Library 40150, replaced in FridaHooks.cpp) returns at once for a
// held form, and the knock step retries it every frame. HoldGetUp refreshes a
// form's hold at each store (it lapses kGetUpHoldMs after the last, so a
// stream that dies never strands a copy down); ReleaseGetUp lifts it at the
// drive's release, the engine's next knock step then getting it up.
// Threadsafe
void HoldGetUp(uint32_t formId);
void ReleaseGetUp(uint32_t formId);

// thuum docs/DECISIONS.md ADR-073 decision 9 (D-L1 B, "ADR-068 decision 6
// as written: every seat whose engine raises the event runs it"): the
// Papyrus event gate's everyone pass on or off (OnSendEventEnter). On, an
// event to a quest's own scripts or to an alias's (reference, location or
// base alias) passes on every seat whose engine raises it, counted; off, the
// default, keeps the gate as it was, so a stage A/Bs one build.
// ScriptNatives::InstallHooks sets it once at kDataLoaded from the client
// setting scriptEveryoneEvents, on only when true. Threadsafe
void SetScriptEveryoneEvents(bool on);
// the gate's pass census since the last call, each nonzero kind one line,
// then zeroed: the platform pass (thuum D-S0-14 B, an event raised inside
// the platform's own call on its thread: "[census] k9-gate-passes passed in
// <s> s: platform <kind> x<n>"), the everyone pass ("[census] k9-gate-passes
// passed in <s> s: everyone alias 140 x<n>") and what it left ("[fall]
// k9-everyone-pass skipped in <s> s: <kind> x<n>"); each kind's first is
// logged once at its pass. ScriptNatives.cpp's FlushCensus, every census
// window
void FlushGatePasses(uint64_t windowMs);

// thuum docs/DECISIONS.md ADR-068 decision 6 ("the player's own (0x14, the
// player's effects, the player-alias quests) are the player's seat's"): this
// seat's own player in the active effect a VM handle names, its target or
// its caster (the gate's own order, the target first, so an effect the
// player cast on itself answers kTarget), or neither, kNone also for a
// handle that is no active effect's
// (CommonLibSSE-NG include/RE/A/ActiveEffect.h:31, VMTYPEID 142;
// IObjectHandlePolicy.h:20 and :27). One test, two callers: the Papyrus
// event gate admits such an effect's events (OnSendEventEnter, ADR-031's
// push), and the seam lets such an effect's script place its helpers
// (ScriptNatives.cpp, thuum train 52's Fix 2), so an effect script places
// wherever the gate lets it run. Two policy calls and a handle read; no lock
// of this file's
enum class PlayersEffectRole : std::uint8_t
{
  kNone,
  kCaster,
  kTarget
};
PlayersEffectRole PlayersEffect(RE::BSScript::IObjectHandlePolicy* policy,
                                RE::VMHandle handle);

// the Undead FX build (thuum ADR-068 decision 6 applied to an actor's aliases
// and effects; the reanimate-fx memo's D5 B): the Papyrus event gate's owner
// rule on or off (OnSendEventEnter). On, the default, one to an active
// effect on an actor that is not the player on the seat that simulates the
// effect's target, each tallied; off, the rule asks nothing and the gate is
// the block's as before.
// ScriptNatives::InstallHooks sets it once at kDataLoaded from the client
// setting scriptOwnerEvents. Threadsafe
void SetScriptOwnerEvents(bool on);
bool ScriptOwnerEventsOn();

// thuum docs/DECISIONS.md ADR-068 D-W3 B (Eli 2026-11-10): the presser rule's
// held entries (an enter to a scripted world trigger it blanked while the
// presser's owner was undecided here, an NPC standing inside at this seat's
// load, kept per (trigger, presser) with a count and a 60 s expiry), each
// replayed once through SendTriggerEvent's body when its presser is this
// seat's now by the rule's own owner test and its trigger stands in a cell
// this seat hosts; the expired ones dropped. TESModPlatform.ReplayHeldEntries'
// body, on the update thread inside callNative (the platform's mark), which
// skymp5-client calls at the update after the one that saw its own hosting
// state change. A JSON string: replayed, refused, expired, held. No
// exception leaves it. Turning the rule off clears the held entries
std::string ReplayHeldEntries();

// thuum docs/research/fifty-fixes-2026-11-07.md 1(b), the furniture branch:
// the animation hook's rule over a furniture's own graph event on or off
// (OnSendAnimationEventEnter). On, an event the engine sends to a plain
// reference whose base is a FURN never reaches the sendAnimationEvent
// handlers and crosses after the call as furnitureAnimation; off, the
// default, keeps every graph event on the handlers as before.
// skymp5-client's setting furnitureAnimation turns it on through
// setFurnitureAnimation (PuppetApi.cpp). Threadsafe
void SetFurnitureAnimation(bool on);

// thuum docs/verbs/quest-stage.md, slice two: a (quest, stage) pair
// skymp5-client's questStageService is applying from the server's relay,
// whose Papyrus fragment the VM's quest stage sink skips at the pair's next
// event (the Gum replacement in FridaHooks.cpp; the journal, the done bit
// and the latent return stay the engine's). TESModPlatform's
// SuppressQuestStageFragment puts one mark on the pair before its SetStage;
// the sink consumes the mark at the pair's first event, so a stage the VM
// queues on an unstarted quest and sets later still finds it;
// ReleaseQuestStageFragment is the client's safety 10 s after the apply,
// taking back a mark no event claimed. Two applies of one pair are two
// marks. Threadsafe.
void SuppressQuestStageFragment(uint32_t questFormId, uint16_t stage);
void ReleaseQuestStageFragment(uint32_t questFormId, uint16_t stage);

// thuum docs/verbs/cast-action.md: the hand whose cast start the engine's
// action entry performed for the player and no cast event has claimed yet
// (0 left, 1 right; the perk's dual cast is the left hand's actions), the
// oldest within withinMs, taken (cleared) by the call, or -1. The cast event
// names no hand, and the casters' states could not tell them apart when both
// held the spell (x-left-firebolt2 20261015-070447), where the action does;
// one pending start per hand keeps two hands casting the same spell apart
// (playtest nineteen: the latest start's hand named both casts, so the
// figure streamed from one hand twice). A hand's Interrupt or Equipped
// action drops its pending start (a cancelled charge fires no cast event).
int32_t TakeCastStart(uint64_t withinMs);

// thuum docs/verbs/cast-action.md: whether the actor's graph has answered a
// cast request for the hand (0 left, 1 right, 2 voice) since the last take:
// the BeginCastLeft, BeginCastRight or BeginCastVoice graph event reaching
// its handler, which is when the engine's own caster performs the Attack
// action. Taken (cleared) by the call. The view performs a figure's start on
// this answer, as the engine does, not on a timer (x-cast-gap
// 20261015-213755: every taken start 4 to 7 ms after the answer; moving
// figures answered later than the view's one update, playtest nineteen).
bool TakeCastReady(uint32_t actorFormId, int32_t hand);

// records an answer (hand bit 1 left, 2 right, 4 voice); called by the
// typed hooks on the response handlers (Hooks.cpp)
void NoteCastReady(uint32_t actorFormId, uint32_t handBit);

// thuum docs/verbs/marksman.md: open on this thread while TESModPlatform's
// LaunchArrow or LaunchArrowAt launches the arrow of a shot the server
// relayed. A figure wears its quiver (skymp5-client equipment.ts), so its bow
// releasing in this game launches an arrow of its own beside the relayed one
// (x-double-arrow2 20261012-022726: two launches for one shot); the launch
// hook lets a figure's arrow through only inside one of these.
struct RelayedArrowScope
{
  RelayedArrowScope();
  ~RelayedArrowScope();
  RelayedArrowScope(const RelayedArrowScope&) = delete;
  RelayedArrowScope& operator=(const RelayedArrowScope&) = delete;
};

// thuum tracker #123 (playtest 55, the spoken-line read): every line the
// engine's DialogueItem::SendTopicInfoEvent (Address Library 35225, the
// fragment gate's hook below) sees for an actor speaker, recorded before the
// gate decides, plus the lines this game's own code ends without the
// engine's END (PuppetApi.cpp SayTopicInfo's pre-clear and its inaudible
// branch). Per speaker the last 64 rows; at most 256 speakers, the least
// recently written dropped first. One mutex; no engine call under it
struct SpokenLine
{
  std::uint64_t epochMs = 0;       // the system clock, ms since the epoch
  std::uint32_t infoId = 0;        // DialogueItem::info, 0 when none
  std::uint32_t topicId = 0;       // DialogueItem::topic, 0 when none
  std::uint32_t type = 0;          // kSpokenBegin, kSpokenEnd or kSpokenCut
  std::uint32_t gated = 0;         // 1: the fragment gate dropped the event
  std::uint32_t handleValid = 0;   // 1: soundHandles[0] held a sound id
  std::int32_t responseIndex = -1; // currentResponse's place, -1 when none
  float voiceTimerSec = 0.f;       // the Actor runtime voiceTimer
};
// a row's type: the engine's TESTopicInfoEvent types (0 BEGIN, 1 END), and
// this game's own cut (2)
inline constexpr std::uint32_t kSpokenBegin = 0;
inline constexpr std::uint32_t kSpokenEnd = 1;
inline constexpr std::uint32_t kSpokenCut = 2;
// records one row for the speaker from the item (null for a row with no
// item's fields). Reads fields only (no relocation call); nothing leaves it
void RecordSpokenLine(RE::Actor* speaker, RE::DialogueItem* item,
                      std::uint32_t type, bool gated) noexcept;
// the speaker's rows with epochMs >= sinceMs, oldest first; a table read,
// no game call, safe from any thread
std::vector<SpokenLine> ReadSpokenLines(std::uint32_t speakerId,
                                        std::uint64_t sinceMs);
}
