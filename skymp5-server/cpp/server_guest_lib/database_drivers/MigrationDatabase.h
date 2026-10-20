#pragma once
#include "MpChangeForms.h"
#include <database_drivers/IDatabase.h>

class MigrationDatabase
  : public Viet::IDatabase<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
public:
  MigrationDatabase(
    std::shared_ptr<
      Viet::IDatabase<MpChangeForm, FormDesc, std::vector<FormDesc>>>
      newDatabase,
    std::shared_ptr<
      Viet::IDatabase<MpChangeForm, FormDesc, std::vector<FormDesc>>>
      oldDatabase,
    std::function<void()> exit = [] { std::exit(0); },
    std::function<void()> terminate = [] { std::terminate(); });
  void Iterate(const IterateCallback& iterateCallback,
               std::optional<std::vector<FormDesc>> filter) override;

  // thuum ADR-043: passed through, the order written to the new database
  // and read from the old one
  void UpsertLoadOrder(const LoadOrder& order) override;
  std::optional<LoadOrder> LoadLoadOrder() override;

private:
  std::vector<std::optional<MpChangeForm>>&& UpsertImpl(
    std::vector<std::optional<MpChangeForm>>&& changeForms,
    size_t& outNumUpserted) override;

  struct Impl;
  std::shared_ptr<Impl> pImpl;
};
