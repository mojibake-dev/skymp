#pragma once
#include "libespm/espm.h"
#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <simdjson.h>
#include <unordered_map>
#include <utility>
#include <vector>

class ActiveMagicEffectsMap
{
public:
  struct Entry
  {
    uint32_t timerId = -1;
    espm::Effects::Effect data;
    std::chrono::system_clock::time_point endTime;
  };

public:
  static ActiveMagicEffectsMap FromJson(const simdjson::dom::element& effects);

public:
  template <typename T>
  void Add(espm::ActorValue actorValue, T&& entry)
  {
    auto it = effects.find(actorValue);
    effects[actorValue] = std::forward<T>(entry);
  }

  std::vector<espm::Effects::Effect> GetAllEffects() const noexcept;

  // thuum docs/verbs/magic-effects.md: every entry with its actor value and
  // end time as kept in memory (the JSON's end time goes through a "%c"
  // string, which does not read back reliably)
  std::vector<std::pair<espm::ActorValue, Entry>> GetEntries() const;

  std::optional<std::reference_wrapper<const Entry>> Get(
    espm::ActorValue actorValue) const noexcept;
  void Remove(espm::ActorValue actorValue) noexcept;
  void Clear() noexcept;
  bool Has(espm::ActorValue actorValue) const noexcept;
  [[nodiscard]] bool Empty() const noexcept;
  nlohmann::json::array_t ToJson() const;

private:
  std::unordered_map<espm::ActorValue, Entry> effects;
};
