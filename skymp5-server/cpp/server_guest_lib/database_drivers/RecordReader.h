#pragma once
#include "MpChangeForms.h"
#include <database_drivers/IDatabase.h>
#include <memory>
#include <optional>
#include <simdjson.h>
#include <spdlog/logger.h>
#include <vector>

// Reads a driver's records as the running load order sees them (thuum
// docs/DECISIONS.md ADR-043; docs/research/formdesc-world-map.md section 6).
// A plugin-relative record resolves against the running order. A raw-shape
// record (no formVersion) is re-based from the order the world was saved
// under, or from the running one when none was saved, and queued; Commit
// writes the queue back as version 2 and the running order beside it, so a
// world is migrated once, at the load that first sees it. Without a running
// order (the tests, the legacy path) records are read as they are.
class RecordReader
{
public:
  using Database =
    Viet::IDatabase<MpChangeForm, FormDesc, std::vector<FormDesc>>;

  RecordReader(const std::shared_ptr<const Database::LoadOrder>& running,
               std::optional<Database::LoadOrder> saved,
               std::shared_ptr<spdlog::logger> logger);

  MpChangeForm Read(simdjson::dom::element& element);

  // After every record: the re-based ones written back, the running order
  // kept beside them, one summary line
  void Commit(Database& database);

  // The running order as MpChangeForm::ToJson takes it; nullopt keeps the
  // raw shape
  static std::optional<EspmFileList> Files(
    const std::shared_ptr<const Database::LoadOrder>& order);

private:
  std::optional<EspmFileList> running;
  std::optional<EspmFileList> saved;
  std::shared_ptr<spdlog::logger> logger;
  std::vector<std::optional<MpChangeForm>> migrated;
  size_t numRead = 0;
  size_t numUnresolved = 0;
};
