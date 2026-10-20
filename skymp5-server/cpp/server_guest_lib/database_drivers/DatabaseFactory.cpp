#include "DatabaseFactory.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "FileDatabase.h"
#include "MigrationDatabase.h"
#include "ZipDatabase.h"
#include <database_drivers/MongoDatabase.h>

std::shared_ptr<DatabaseFactory::Database> DatabaseFactory::Create(
  nlohmann::json settings, std::shared_ptr<spdlog::logger> logger,
  std::shared_ptr<const LoadOrder> loadOrder)
{
  auto databaseDriver = settings.count("databaseDriver")
    ? settings["databaseDriver"].get<std::string>()
    : std::string("file");

  if (databaseDriver == "file") {
    auto databaseName = settings.count("databaseName")
      ? settings["databaseName"].get<std::string>()
      : std::string("world");

    logger->info("Using file with name '" + databaseName + "'");
    auto database = std::make_shared<FileDatabase>(databaseName, logger);
    database->SetLoadOrder(loadOrder);
    return database;
  }

  if (databaseDriver == "mongodb") {
    auto databaseName = settings.count("databaseName")
      ? settings["databaseName"].get<std::string>()
      : std::string("db");

    auto databaseUri = settings["databaseUri"].get<std::string>();
    logger->info("Using mongodb with name '" + databaseName + "'");
    auto database = std::make_shared<MongoDatabase>(databaseUri, databaseName);
    database->SetLoadOrder(loadOrder);
    return database;
  }

  if (databaseDriver == "migration") {
    // both sides under the running order: the old one is read into it and
    // the new one written against it (thuum ADR-043)
    auto from = settings.at("databaseOld");
    auto to = settings.at("databaseNew");
    auto oldDatabase = Create(from, logger, loadOrder);
    auto newDatabase = Create(to, logger, loadOrder);
    auto database =
      std::make_shared<MigrationDatabase>(newDatabase, oldDatabase);
    database->SetLoadOrder(loadOrder);
    return database;
  }

  if (databaseDriver == "zip") {
    auto databaseName = settings.count("databaseName")
      ? settings["databaseName"].get<std::string>()
      : std::string("world");

    logger->info("Using zip with name '" + databaseName + "'");
    auto database = std::make_shared<ZipDatabase>(databaseName, logger);
    database->SetLoadOrder(loadOrder);
    return database;
  }

  throw std::runtime_error("Unrecognized databaseDriver: " + databaseDriver);
}
