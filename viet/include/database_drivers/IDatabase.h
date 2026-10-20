#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Viet {

template <typename T, typename FormDescType, typename FilterType>
class IDatabase
{
public:
  using IterateCallback = std::function<void(const T&)>;

  // The plugins a world's records are written against: each file name with
  // whether the engine loads it as a light plugin, in load order (thuum
  // docs/DECISIONS.md ADR-043)
  using LoadOrder = std::vector<std::pair<std::string, bool>>;

  virtual ~IDatabase() = default;

  // Returns numbers of change forms inserted or updated successfully (Suitable
  // for logging). In practice, it should be equal to `changeForms.size()` when
  // saving succeed.
  size_t Upsert(std::vector<std::optional<T>>&& changeForms)
  {
    size_t numUpserted = 0;
    recycledChangeFormsBuffer =
      UpsertImpl(std::move(changeForms), numUpserted);
    return numUpserted;
  }

  virtual void Iterate(const IterateCallback& iterateCallback,
                       std::optional<FilterType> filter) = 0;

  // The order the running server loads (ADR-043). With it a driver writes
  // every stored form plugin-relative and resolves it back on read; without
  // it (the tests, the legacy path) records keep the raw shape.
  void SetLoadOrder(std::shared_ptr<const LoadOrder> order)
  {
    loadOrder = std::move(order);
  }

  const std::shared_ptr<const LoadOrder>& GetLoadOrder() const noexcept
  {
    return loadOrder;
  }

  // The order a world was saved under, kept beside its records (ADR-043
  // decision 2): read before the records, so one in the raw shape is
  // re-based from it; written once the records are in the plugin-relative
  // shape. nullopt is a world that never recorded one.
  virtual void UpsertLoadOrder(const LoadOrder& order) = 0;
  virtual std::optional<LoadOrder> LoadLoadOrder() = 0;

  bool GetRecycledChangeFormsBuffer(std::vector<std::optional<T>>& changeForms)
  {
    if (recycledChangeFormsBuffer.empty()) {
      return false;
    }

    for (auto& value : recycledChangeFormsBuffer) {
      value = std::nullopt;
    }

    changeForms = std::move(recycledChangeFormsBuffer);
    recycledChangeFormsBuffer.clear();
    return true;
  }

protected:
  virtual std::vector<std::optional<T>>&& UpsertImpl(
    std::vector<std::optional<T>>&& changeForms, size_t& outNumUpserted) = 0;

  std::vector<std::optional<T>> recycledChangeFormsBuffer;
  std::shared_ptr<const LoadOrder> loadOrder;
};

}
