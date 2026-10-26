#pragma once

#include "AnimVariableMasterGraphIndexes.h"
#include "hkbVariableValueSet.h"
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <type_traits>
#include <vector>

class AnimationGraphMasterBehaviourDescriptor final
{

public:
  struct AnimationVariables final
  {
  public:
    std::vector<uint8_t> booleans{};
    std::vector<float> floats{};
    std::vector<uint32_t> integers{};

  public:
    struct InitData final
    {
      uint8_t* ptr = nullptr;
      size_t numBytes = 0;
    };

    using BooleanAnimVarType = decltype(booleans)::value_type;
    using FloatAnimVarType = decltype(floats)::value_type;
    using IntegerAnimVarType = decltype(integers)::value_type;

    AnimationVariables() = default;

    explicit AnimationVariables(InitData booleanInitData,
                                InitData floatInitData,
                                InitData integerInitData)
    {
      if (booleanInitData.numBytes > 0 && booleanInitData.ptr) {
        const size_t numBooleans =
          booleanInitData.numBytes / sizeof(BooleanAnimVarType);

        booleans.reserve(numBooleans);

        for (size_t i = 0; i < numBooleans; ++i) {
          booleans.emplace_back(*reinterpret_cast<BooleanAnimVarType*>(
            booleanInitData.ptr + (i * sizeof(BooleanAnimVarType))));
        }
      }

      if (floatInitData.numBytes > 0 && floatInitData.ptr) {
        const size_t numFloats =
          floatInitData.numBytes / sizeof(FloatAnimVarType);

        floats.reserve(numFloats);

        for (size_t i = 0; i < numFloats; ++i) {
          floats.emplace_back(*reinterpret_cast<FloatAnimVarType*>(
            floatInitData.ptr + (i * sizeof(FloatAnimVarType))));
        }
      }

      if (integerInitData.numBytes > 0 && integerInitData.ptr) {
        const size_t numIntegers =
          integerInitData.numBytes / sizeof(IntegerAnimVarType);

        integers.reserve(numIntegers);

        for (size_t i = 0; i < numIntegers; ++i) {
          integers.emplace_back(*reinterpret_cast<IntegerAnimVarType*>(
            integerInitData.ptr + (i * sizeof(IntegerAnimVarType))));
        }
      }
    }

    [[nodiscard]] bool IsEmpty() const noexcept
    {
      return booleans.empty() && floats.empty() && integers.empty();
    }

    [[nodiscard]] uint32_t SizeBooleansInBytes() const noexcept
    {
      return booleans.size() * sizeof(BooleanAnimVarType);
    }

    [[nodiscard]] uint32_t SizeFloatsInBytes() const noexcept
    {
      return floats.size() * sizeof(FloatAnimVarType);
    }

    [[nodiscard]] uint32_t SizeIntegersInBytes() const noexcept
    {
      return integers.size() * sizeof(IntegerAnimVarType);
    }
  };

  explicit AnimationGraphMasterBehaviourDescriptor(
    AnimationVariables _variables)
    : variables(std::move(_variables))
  {
  }

  AnimationVariables GetVariables() const { return variables; }

  explicit AnimationGraphMasterBehaviourDescriptor(const RE::Actor& actor)
  {
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> pManager;

    if (actor.GetAnimationGraphManager(pManager) == false) {
      return;
    }

    RE::BSSpinLockGuard _{ pManager->GetRuntimeData().updateLock };

    if (pManager->GetRuntimeData().activeGraph >= pManager->graphs.size()) {
      return;
    }

    const RE::BShkbAnimationGraph* pGraph = actor.formID == 0x14
      ? pManager->graphs[0].get()
      : pManager->graphs[pManager->GetRuntimeData().activeGraph].get();

    if (pGraph == nullptr || pGraph->behaviorGraph == nullptr ||
        pGraph->behaviorGraph->rootGenerator.get() == nullptr ||
        pGraph->behaviorGraph->rootGenerator->name.data() == nullptr) {
      return;
    }

    const auto* pVariableSet =
      reinterpret_cast<hkbVariableValueSet<uint32_t>*>(
        pGraph->behaviorGraph->variableValueSet.get());

    if (!pVariableSet) {
      return;
    }
    if (!IsMasterBehaviourGraph(*pGraph, *pVariableSet, actor.formID)) {
      return;
    }

    variables.booleans.resize(agDescriptor.boolVariableIndexes.size());
    variables.floats.resize(agDescriptor.floatVariableIndexes.size());
    variables.integers.resize(agDescriptor.intVariableIndexes.size());

    for (size_t i = 0; i < agDescriptor.boolVariableIndexes.size(); ++i) {
      variables.booleans[i] =
        *reinterpret_cast<AnimationVariables::BooleanAnimVarType*>(
          &pVariableSet->varSet[agDescriptor.boolVariableIndexes[i]]);
    }

    for (size_t i = 0; i < agDescriptor.floatVariableIndexes.size(); ++i) {
      variables.floats[i] =
        *reinterpret_cast<AnimationVariables::FloatAnimVarType*>(
          &pVariableSet->varSet[agDescriptor.floatVariableIndexes[i]]);
    }

    for (size_t i = 0; i < agDescriptor.intVariableIndexes.size(); ++i) {
      variables.integers[i] =
        *reinterpret_cast<AnimationVariables::IntegerAnimVarType*>(
          &pVariableSet->varSet[agDescriptor.intVariableIndexes[i]]);
    }
  }

  [[nodiscard]] bool ApplyVariablesToActor(const RE::Actor& actor) const
  {
    if (variables.IsEmpty()) {
      return false;
    }

    RE::BSTSmartPointer<RE::BSAnimationGraphManager> pManager;

    if (actor.GetAnimationGraphManager(pManager) == false) {
      return false;
    }

    RE::BSSpinLockGuard _{ pManager->GetRuntimeData().updateLock };

    if (pManager->GetRuntimeData().activeGraph >= pManager->graphs.size()) {
      return false;
    }

    const RE::BShkbAnimationGraph* pGraph = actor.formID == 0x14
      ? pManager->graphs[0].get()
      : pManager->graphs[pManager->GetRuntimeData().activeGraph].get();

    if (pGraph == nullptr || pGraph->behaviorGraph == nullptr ||
        pGraph->behaviorGraph->rootGenerator.get() == nullptr ||
        pGraph->behaviorGraph->rootGenerator->name.data() == nullptr) {
      return false;
    }

    const auto* pVariableSet =
      reinterpret_cast<hkbVariableValueSet<uint32_t>*>(
        pGraph->behaviorGraph->variableValueSet.get());

    if (!pVariableSet) {
      return false;
    }
    if (!IsMasterBehaviourGraph(*pGraph, *pVariableSet, actor.formID)) {
      return false;
    }

    for (size_t i = 0; i < agDescriptor.boolVariableIndexes.size(); ++i) {
      *reinterpret_cast<AnimationVariables::BooleanAnimVarType*>(
        &pVariableSet->varSet[agDescriptor.boolVariableIndexes[i]]) =
        variables.booleans.size() > i ? variables.booleans[i] : 0;
    }

    for (size_t i = 0; i < agDescriptor.floatVariableIndexes.size(); ++i) {
      *reinterpret_cast<AnimationVariables::FloatAnimVarType*>(
        &pVariableSet->varSet[agDescriptor.floatVariableIndexes[i]]) =
        variables.floats.size() > i ? variables.floats[i] : 0.f;
    }

    for (size_t i = 0; i < agDescriptor.intVariableIndexes.size(); ++i) {
      *reinterpret_cast<AnimationVariables::IntegerAnimVarType*>(
        &pVariableSet->varSet[agDescriptor.intVariableIndexes[i]]) =
        variables.integers.size() > i ? variables.integers[i] : 0;
    }

    return true;
  }

private:
  // thuum docs/verbs/npc-stream.md (slice 1 v2, 2026-10-26): the indexes
  // this descriptor reads and writes are the humanoid master behaviour's
  // (AnimationVariablesForMasterBehavior.h), and every figure's graph was
  // that one. A hosted NPC's graph may be another project (ChickenProject,
  // HighlandCowProject: the puppet seat's three crashes on the first stage,
  // x-npc-stream-p4 to -p6 20261026-1519 to -1528, the master's indexes
  // written into a creature's variable set), so a graph is admitted only
  // when its project is the master behaviour's (DefaultMale or
  // DefaultFemale; HYPOTHESIS on the names until the lab's log line reads
  // them) and every index lies inside its variable set. A refused project
  // is logged once.
  bool IsMasterBehaviourGraph(const RE::BShkbAnimationGraph& graph,
                              const hkbVariableValueSet<uint32_t>& set,
                              uint32_t actorFormId) const
  {
    const char* project = graph.projectName.c_str();
    const std::string name = project ? project : "";
    const bool master = _stricmp(name.c_str(), "DefaultMale") == 0 ||
      _stricmp(name.c_str(), "DefaultFemale") == 0;
    bool inside = master;
    if (inside) {
      for (const auto idx : agDescriptor.boolVariableIndexes) {
        inside = inside && idx < set.size;
      }
      for (const auto idx : agDescriptor.floatVariableIndexes) {
        inside = inside && idx < set.size;
      }
      for (const auto idx : agDescriptor.intVariableIndexes) {
        inside = inside && idx < set.size;
      }
    }
    if (!master || !inside) {
      static std::mutex m;
      static std::set<std::string> said;
      std::lock_guard<std::mutex> lock(m);
      if (said.insert(name).second) {
        logger::info("[verb:npc-stream] graph project '{}' (variable set of "
                     "{}) is not the master behaviour: no variables read or "
                     "written for {:x} or any actor on it",
                     name, set.size, actorFormId);
      }
    }
    return master && inside;
  }

  AnimationVariables variables{};

  AnimVariableMasterGraphIndexes agDescriptor =
    AnimVariableMasterGraphIndexes::CreateDefault();
};

namespace skymp {
template <typename T>
struct is_vector : std::false_type
{
};

template <typename T, typename Alloc>
struct is_vector<std::vector<T, Alloc>> : std::true_type
{
};
} // namespace skymp

static_assert(
  skymp::is_vector<decltype(AnimationGraphMasterBehaviourDescriptor::
                              AnimationVariables::booleans)>::value,
  "Member of AnimationVariables->Booleans must be std::vector<T>");

static_assert(
  skymp::is_vector<decltype(AnimationGraphMasterBehaviourDescriptor::
                              AnimationVariables::floats)>::value,
  "Member of AnimationVariables->Floats must be std::vector<T>");

static_assert(
  skymp::is_vector<decltype(AnimationGraphMasterBehaviourDescriptor::
                              AnimationVariables::integers)>::value,
  "Member of AnimationVariables->Integers must be std::vector<T>");
