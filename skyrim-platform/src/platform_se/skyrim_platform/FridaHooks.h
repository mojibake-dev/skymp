#pragma once

#include <RE/B/BSCoreTypes.h>
#include <cstdint>
#include <string>

namespace RE {
class Actor;
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
}
