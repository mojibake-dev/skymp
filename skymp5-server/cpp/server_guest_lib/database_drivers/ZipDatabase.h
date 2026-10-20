#pragma once
#include "MpChangeForms.h"
#include <database_drivers/IDatabase.h>
#include <spdlog/spdlog.h>

class ZipDatabase
  : public Viet::IDatabase<MpChangeForm, FormDesc, std::vector<FormDesc>>
{
public:
  ZipDatabase(std::string filePath_, std::shared_ptr<spdlog::logger> logger_);
  ~ZipDatabase();

  void Iterate(const IterateCallback& iterateCallback,
               std::optional<std::vector<FormDesc>> filter) override;

  // thuum ADR-043: the load-order.json entry of the archive
  void UpsertLoadOrder(const LoadOrder& order) override;
  std::optional<LoadOrder> LoadLoadOrder() override;

private:
  std::vector<std::optional<MpChangeForm>>&& UpsertImpl(
    std::vector<std::optional<MpChangeForm>>&& changeForms,
    size_t& outNumUpserted) override;

  struct Impl;
  std::shared_ptr<Impl> pImpl;
};
