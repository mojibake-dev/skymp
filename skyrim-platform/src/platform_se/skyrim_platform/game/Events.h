#pragma once

#include "events/ItemHarvested.h"
#include "events/PoisonedWeapon.h"

namespace RE {

struct TESQuestInitEvent
{
  RE::FormID questId;
};

struct TESQuestStageItemDoneEvent // not finished
{
  uint32_t stage;
  RE::FormID questId;
  void* one;     // ? pointer
  void* two;     // pointer?
  uint32_t flag; // 1, -1 ?
};

struct TESTriggerEvent
{
  RE::NiPointer<RE::TESObjectREFR> target;
  RE::NiPointer<RE::TESObjectREFR> caster;
};

struct TESTriggerEnterEvent
{
  RE::NiPointer<RE::TESObjectREFR> target;
  RE::NiPointer<RE::TESObjectREFR> caster;
};

struct TESTriggerLeaveEvent
{
  RE::NiPointer<RE::TESObjectREFR> target;
  RE::NiPointer<RE::TESObjectREFR> caster;
};

struct TESSleepStartEvent
{
  float sleepStartTime;
  float desiredSleepEndTime;
};

struct TESWaitStartEvent
{
  float waitStartTime;
  float desiredWaitEndTime;
};

struct TESSellEvent
{
  RE::NiPointer<RE::TESObjectREFR> target;
  RE::NiPointer<RE::TESObjectREFR> seller;
};

struct TESCellReadyToApplyDecalsEvent
{
  RE::NiPointer<RE::TESObjectCELL> cell;
};

struct TESMagicWardHitEvent
{
  enum class Status : uint32_t
  {
    kFriendly = 0,
    kAbsorbed = 1,
    kBroken = 2
  };
  RE::NiPointer<RE::TESObjectREFR> target;
  RE::NiPointer<RE::TESObjectREFR> caster;
  RE::FormID spell;
  Status status;
};

struct TESPackageEvent
{
  enum class EventType : uint32_t // not sure
  {
    kStart = 0,
    kChange = 1,
    kEnd = 2
  };
  RE::NiPointer<RE::TESObjectREFR> actor;
  RE::FormID package;
  EventType type;
};

struct TESDestructionStageChangedEvent
{
  RE::NiPointer<RE::TESObjectREFR> target;
  uint32_t oldStage;
  uint32_t newStage;
};

// thuum docs/verbs/npc-stream.md (the voice slice): the engine's event for
// a spoken line, which CommonLibSSE-NG at the pin forward-declares alone
// (ScriptEventSourceHolder.h:54). The layout is the re-analyst's REA read
// of 1.6.1170 on sky-re (2026-10-26, CONFIRMED-STATIC from the dispatcher
// 25616 and SkyrimVM's consumer 53997): a refcounted callback pointer (null
// for a bark), the speaker, the topic info's form id, the type (0 the
// line's begin, 1 its end)
struct TESTopicInfoEvent
{
  void* callback;
  RE::NiPointer<RE::TESObjectREFR> speaker;
  RE::FormID topicInfoFormID;
  std::uint32_t type;
};
static_assert(sizeof(TESTopicInfoEvent) == 0x18);

struct TESSceneActionEvent
{
  void* reference;
  RE::FormID sceneId;
  uint32_t actionIndex;
  RE::FormID questId;
  uint32_t actorAliasId;
};

struct TESSceneEvent
{
  RE::TESForm* reference; // what ref?
  RE::FormID sceneId;
};

struct TESObjectREFRTranslationEvent
{
  enum class EventType : uint32_t
  {
    kFailed = 0,
    kCompleted = 1,
    kAlmostCompleted = 2
  };
  RE::NiPointer<RE::TESObjectREFR> refr;
  EventType type;
};

struct TESTrapHitEvent // not finished
{
  RE::NiPointer<RE::TESObjectREFR> trap;
  RE::NiPointer<RE::TESObjectREFR> target;
  uint32_t flag; // flag ? 0 - traphit, 1 - traphitend, 2 - traphitstart
  float f1;
  float f2;
  float f3;
  float f4;
  float f5;
  float f6;
  float f7; // 120 damage
  float f8;
  float f9;
  float f10;
  float f11;
  float f12;
  float f13;
  float f14;
};

struct TESPerkEntryRunEvent
{
  RE::NiPointer<RE::TESObjectREFR> cause;
  RE::NiPointer<RE::TESObjectREFR> target;
  RE::FormID perkId;
  uint32_t flag;
};

}
