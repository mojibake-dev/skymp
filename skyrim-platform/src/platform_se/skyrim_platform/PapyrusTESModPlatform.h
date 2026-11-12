#pragma once

#include <functional>

#include "PCH.h"

namespace TESModPlatform {
extern std::function<void(IVM* vm, StackID stackId)> onPapyrusUpdate;

int32_t Add(IVM* vm, StackID stackId, RE::StaticFunctionTag*, int32_t, int32_t,
            int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
            int32_t, int32_t, int32_t);

void MoveRefrToPosition(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                        RE::TESObjectREFR* refr, RE::TESObjectCELL* cell,
                        RE::TESWorldSpace* world, float posX, float posY,
                        float posZ, float rotX, float rotY, float rotZ);

enum
{
  WEAP_DRAWN_MODE_DEFAULT = -1,
  WEAP_DRAWN_MODE_ALWAYS_FALSE = 0,
  WEAP_DRAWN_MODE_ALWAYS_TRUE = 1,

  WEAP_DRAWN_MODE_MIN = WEAP_DRAWN_MODE_DEFAULT,
  WEAP_DRAWN_MODE_MAX = WEAP_DRAWN_MODE_ALWAYS_TRUE
};

void SetWeaponDrawnMode(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                        RE::Actor* actor, int32_t weapDrawnMode);

int32_t GetNthVtableElement(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                            RE::TESForm* pointer, int32_t pointerOffset,
                            int32_t elementIndex);

bool IsPlayerRunningEnabled(IVM* vm, StackID stackId, RE::StaticFunctionTag*);

RE::BGSColorForm* GetSkinColor(IVM* vm, StackID stackId,
                               RE::StaticFunctionTag*, RE::TESNPC* base);

RE::TESNPC* CreateNpc(IVM* vm, StackID stackId, RE::StaticFunctionTag*);

RE::TESNPC* EvaluateLeveledNpc(IVM* vm, StackID stackId,
                               RE::StaticFunctionTag*,
                               FixedString commaSeparatedListOfIds);

void SetNpcSex(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
               RE::TESNPC* npc, int32_t sex);

void SetNpcRace(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                RE::TESNPC* npc, RE::TESRace* race);

void SetNpcSkinColor(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                     RE::TESNPC* npc, int32_t skinColor);

void SetNpcHairColor(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                     RE::TESNPC* npc, int32_t skinColor);

void ResizeHeadpartsArray(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                          RE::TESNPC* npc, int8_t size);

void ResizeTintsArray(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                      int32_t size);

void SetFormIdUnsafe(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                     RE::TESForm* form, uint32_t newId);

void ClearTintMasks(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                    RE::Actor* targetActor);

void PushTintMask(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                  RE::Actor* targetActor, int32_t type, uint32_t argb,
                  FixedString texturePath);

void PushWornState(IVM* vm, StackID stackId, RE::StaticFunctionTag*, bool worn,
                   bool wornLeft);

void AddItemEx(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
               RE::TESObjectREFR* containerRefr, RE::TESForm* item,
               int32_t countDelta, float health,
               RE::EnchantmentItem* enchantment, int32_t maxCharge,
               bool removeEnchantmentOnUnequip, float chargePercent,
               FixedString textDisplayData, int32_t soul,
               RE::AlchemyItem* poison, int32_t poisonCount);

void UpdateEquipment(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                     RE::Actor* containerRefr, RE::TESForm* item,
                     bool leftHand);

void ResetContainer(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                    RE::TESForm* container);
void CloseMenu(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
               std::string_view name);

// thuum docs/DECISIONS.md ADR-059 decision 1 and ADR-062 decision 1: a cell
// this game hosts (or no longer hosts), where every world script's events
// pass the Papyrus event block and the script hooks count a call as the
// host's (Frida::SetCellHosted); skymp5-client's hostService calls it at
// every grant and release
void SetCellHosted(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                   int32_t cellFormId, bool hosted);

// Sets the engine's day count so that GameDaysPassed reads daysPassed and
// runs on from it (thuum docs/verbs/time.md)
void SetGameDaysPassed(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                       float daysPassed);

// A neighbour's arrow, launched from its figure along its shooter's aim
// (thuum docs/verbs/marksman.md): the figure's weapon node, a crossbow's
// magic node, as CommonLibSSE-NG's Projectile::LaunchArrow finds it, with
// the draw's power; false when an argument is missing or not a number
bool LaunchArrow(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                 RE::Actor* shooter, RE::TESObjectWEAP* weapon,
                 RE::TESAmmo* ammo, float power, float aimAngle,
                 float aimHeading);
// The same arrow launched at the origin and with the pitch and heading the
// shooter's own engine used (its projectileLaunched event), so both screens
// fly one path (thuum docs/verbs/marksman.md)
bool LaunchArrowAt(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                   RE::Actor* shooter, RE::TESObjectWEAP* weapon,
                   RE::TESAmmo* ammo, float power, float angleX, float angleZ,
                   float originX, float originY, float originZ);

// The player's favorites as (form id, hotkey) pairs, hotkey -1 for none, and
// marking one with a hotkey, -1 for none (thuum docs/verbs/favorites.md)
std::vector<int32_t> GetFavorites(IVM* vm, StackID stackId,
                                  RE::StaticFunctionTag*);
bool SetFavorite(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                 RE::TESForm* form, int32_t hotkey);

// The player's actor values and progress (thuum docs/verbs/actor-values.md):
// its 164 base values by actor value; one set; and its progress as xp,
// threshold, level, per skill level, xp and threshold, per skill its
// legendary count, read and written
std::vector<float> GetActorValueBases(IVM* vm, StackID stackId,
                                      RE::StaticFunctionTag*);
bool SetActorValueBase(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                       int32_t av, float base);
std::vector<float> GetPlayerProgress(IVM* vm, StackID stackId,
                                     RE::StaticFunctionTag*);
bool SetPlayerSkill(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                    int32_t skill, float level, float xp, float threshold,
                    int32_t legendary);
bool SetPlayerExperience(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                         float xp, float threshold, int32_t level);

// The server's magic effects in the player's own game (thuum
// docs/verbs/magic-effects.md): while blocked, its engine applies no effect
// of a potion, a food or a poison to the player (refused at the player's
// MagicTarget::AddTarget, Hooks.cpp), and the server's buffs reach its actor
// values through ModPlayerActorValue (ActorValueOwner::ModActorValue, the
// change Papyrus ModActorValue makes)
void SetPotionEffectsBlocked(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                             bool blocked);
bool ArePotionEffectsBlocked();
bool ModPlayerActorValue(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                         int32_t av, float delta);
// The hit art beside the hit shader (thuum docs/verbs/magic-effects.md; the
// reanimation look's D2 B, thuum lab/.cache/drafts/reanimate-fx.md 4): an
// effect's hit effect art played on refr as the engine plays one
// (TESObjectREFR::ApplyArtObject), for duration seconds, -1 until stopped, a
// second play of the art on refr replacing this native's first; and the arts
// this native played on refr stopped, never one an engine effect plays
// there. False when nothing was played, or nothing stopped
bool ApplyArtObject(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                    RE::TESObjectREFR* refr, RE::BGSArtObject* art,
                    float duration);
bool StopArtObject(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                   RE::TESObjectREFR* refr, RE::BGSArtObject* art);

// The server's stagger in this game (thuum docs/verbs/stagger.md): the
// engine's own start of a stagger on `target` at `magnitude` from `source`,
// as a hit's reaction calls it; false where it was not called
bool Stagger(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
             RE::Actor* target, float magnitude, RE::Actor* source);

// The Story Manager's Change Location event for `actor`, from `oldLocation`
// (none allowed) to `newLocation` (none reads the actor's current location),
// queued through the engine's own story event queue as its arrival sender
// queues it (thuum docs/verbs/quest-stage.md H-56a,
// docs/research/m3/re-story-location.md): a seat that logs in inside a
// location raises none by itself, so the Story Manager starts none of the
// location's quests there. False where nothing was queued
bool SendStoryChangeLocation(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                             RE::Actor* actor, RE::BGSLocation* oldLocation,
                             RE::BGSLocation* newLocation);
// The world's quest stages without their fragments (thuum
// docs/verbs/quest-stage.md, slice two): a (quest, stage) pair the client is
// applying from the server's relay, in before its SetStage and out at the
// latent call's answer; while in, the VM's quest stage sink fires the stage's
// callback and skips its Papyrus fragment (FridaHooks.cpp). False for no
// quest or a stage outside the engine's u16
bool SuppressQuestStageFragment(IVM* vm, StackID stackId,
                                RE::StaticFunctionTag*, RE::TESQuest* quest,
                                int32_t stage);
bool ReleaseQuestStageFragment(IVM* vm, StackID stackId,
                               RE::StaticFunctionTag*, RE::TESQuest* quest,
                               int32_t stage);

// thuum docs/verbs/change-form.md (ADR-065, system 1): the reference's change
// form, serialized on demand by the engine's own SaveGame on the owner's seat
// (a JSON string: formType, flags, version, bodyHex, fnv1a, the remap rows;
// "" for null, the reason in skyrim-platform.log), and a served record
// applied to this seat's loaded copy through the engine's own LoadGame and
// finish pass (a JSON string: applied, reason, fnvPatched). The JSON shapes
// and the premises are ChangeForm.h's. remapRows is "offset:formId:index,..."
// since callNative takes no array; flags is the sender's u32 as a Papyrus
// Int (`flags | 0` in JS)
RE::BSFixedString ApplyChangeFormResult(IVM* vm, StackID stackId,
                                        RE::StaticFunctionTag*,
                                        RE::TESObjectREFR* refr);
RE::BSFixedString SerializeChangeForm(IVM* vm, StackID stackId,
                                      RE::StaticFunctionTag*,
                                      RE::TESObjectREFR* refr);
RE::BSFixedString ApplyChangeForm(IVM* vm, StackID stackId,
                                  RE::StaticFunctionTag*,
                                  RE::TESObjectREFR* refr,
                                  std::string_view bodyHex, int32_t flags,
                                  int32_t version, std::string_view remapRows);

// thuum docs/DECISIONS.md ADR-068 rung 3 (the script record, amendment A;
// docs/research/rung3-script-record-2026-11-06.md 1(a)): the reference's
// attached scripts read as the record carries them (a JSON string: formId,
// quiescent, fnv1a, seatLocal, scripts; or quiescent false and busy; "" when
// refused, the reason in skyrim-platform.log), a served record's scripts
// written over this seat's copies (a JSON string: applied, retry, reason,
// scripts, resolved, none, unresolved, seatLocal, stacksOnObject, holdUs,
// blanked, fnvAfter), and a lifecycle event (OnCellAttach, OnLoad,
// OnCellLoad) sent to its scripts and relayed as the engine's sinks raise it
// (a JSON string: sent, seen, scripts, reason). The shapes and the premises
// are ScriptState.h's. scriptsJson is the record's scripts as a JSON array
// since callNative takes no array
RE::BSFixedString SerializeScriptState(IVM* vm, StackID stackId,
                                       RE::StaticFunctionTag*,
                                       RE::TESObjectREFR* refr);
RE::BSFixedString ApplyScriptState(IVM* vm, StackID stackId,
                                   RE::StaticFunctionTag*,
                                   RE::TESObjectREFR* refr,
                                   std::string_view scriptsJson);
RE::BSFixedString SendScriptEvent(IVM* vm, StackID stackId,
                                  RE::StaticFunctionTag*,
                                  RE::TESObjectREFR* refr,
                                  std::string_view name, bool unseenOnly);
// thuum docs/DECISIONS.md ADR-068 D-C2 A (docs/research/
// fifty-fixes-2026-11-07.md 1(b)): a relayed trigger entry, OnTriggerEnter
// or OnTriggerLeave sent to the trigger's scripts and relayed to its aliases
// with actionRef as akActionRef, SendScriptEvent's mechanism (a JSON string:
// formId, event, sent, scripts, reason; ScriptState.h)
RE::BSFixedString SendTriggerEvent(IVM* vm, StackID stackId,
                                   RE::StaticFunctionTag*,
                                   RE::TESObjectREFR* trigger,
                                   std::string_view name,
                                   RE::TESObjectREFR* actionRef);
// thuum docs/DECISIONS.md ADR-068 D-W3 B (Eli 2026-11-10): the presser rule's
// held entries (an NPC's enter to a world trigger blanked at this seat's own
// load, its owner undecided then) whose presser is this seat's now, with the
// trigger in a cell this seat hosts, each replayed once through
// SendTriggerEvent's body under the platform's mark; the expired ones dropped
// (a JSON string: replayed, refused, expired, held; FridaHooks.cpp)
RE::BSFixedString ReplayHeldEntries(IVM* vm, StackID stackId,
                                    RE::StaticFunctionTag*);
// thuum docs/DECISIONS.md ADR-068 rung 5 (docs/research/
// rung5-quests-2026-11-09.md 1(c) and 8(b); D-V1 B, D-T2 B): a quest's or a
// reference's script variables read by name for ScriptVariables 81 (a JSON
// string: per key { form, script, var, k, i|f|b } or { form, script, var,
// reason }) and written by name through Variable's setters (a JSON string:
// per entry { form, script, var, written, reason? }); the shapes are
// ScriptState.h's. keysJson and entriesJson are JSON arrays since callNative
// takes no array
RE::BSFixedString ReadScriptVariables(IVM* vm, StackID stackId,
                                      RE::StaticFunctionTag*,
                                      std::string_view keysJson);
RE::BSFixedString WriteScriptVariables(IVM* vm, StackID stackId,
                                       RE::StaticFunctionTag*,
                                       std::string_view entriesJson);

// thuum docs/DECISIONS.md ADR-068 rung 4 (the state rows, amendment B;
// docs/research/rung4-state-rows-2026-11-07.md 1(a)): the world globals this
// seat's engine changed since its last answer, the world streamer's
// WorldGlobal 80. A JSON array as a string, [[formId, value], ...] (the
// form id a number, the value the global's float), "[]" for none: at most
// 512 entries (skymp-wire's WORLD_GLOBALS), any past them answered by a
// later call, each answered value the baseline from then on; a constant, a
// non-finite value and the comma-separated hex ids in `exclude` ("0x"
// optional) never answered. With `reset`, the baseline is every other
// global's value now and the answer "[]"; a call before any reset diffs
// against an empty baseline, so it answers every such global
RE::BSFixedString DiffWorldGlobals(IVM* vm, StackID stackId,
                                   RE::StaticFunctionTag*, bool reset,
                                   std::string_view exclude);

void BlockPapyrusEvents(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                        bool blocked);

RE::TESObjectREFR* CreateReferenceAtLocation(
  IVM* vm, StackID stackId, RE::StaticFunctionTag*, RE::TESForm* baseForm,
  RE::TESObjectCELL* cell, RE::TESWorldSpace* world, float posX, float posY,
  float posZ, float rotX, float rotY, float rotZ, bool persist);

// Threadsafe
void BlockMoveRefrToPosition(bool blocked);
int GetWeapDrawnMode(uint32_t actorId);
uint64_t GetNumPapyrusUpdates();
std::shared_ptr<RE::BSTArray<RE::TintMask*>> GetTintsFor(uint32_t actorId);
bool GetPapyrusEventsBlocked();

void Update();

bool Register(IVM* vm);
}
