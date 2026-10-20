#pragma once
#include "MpChangeForms.h"
#include <database_drivers/IDatabase.h>
#include <memory>
#include <spdlog/spdlog.h>

class DatabaseFactory
{
public:
  using Database =
    Viet::IDatabase<MpChangeForm, FormDesc, std::vector<FormDesc>>;
  using LoadOrder = Database::LoadOrder;

  // loadOrder is the order the server runs (thuum ADR-043); the driver it
  // makes writes records against it and reads them back into it. Null keeps
  // the raw shape.
  static std::shared_ptr<Database> Create(
    nlohmann::json settings, std::shared_ptr<spdlog::logger> logger,
    std::shared_ptr<const LoadOrder> loadOrder = nullptr);
};
