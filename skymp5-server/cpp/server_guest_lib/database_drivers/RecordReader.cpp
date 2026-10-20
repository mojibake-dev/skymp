#include "RecordReader.h"
#include <spdlog/spdlog.h>

RecordReader::RecordReader(
  const std::shared_ptr<const Database::LoadOrder>& running_,
  std::optional<Database::LoadOrder> saved_,
  std::shared_ptr<spdlog::logger> logger_)
  : running(Files(running_))
  , logger(logger_ ? std::move(logger_) : spdlog::default_logger())
{
  if (saved_) {
    saved = EspmFileList::FromLoadOrder(*saved_);
  }
}

std::optional<EspmFileList> RecordReader::Files(
  const std::shared_ptr<const Database::LoadOrder>& order)
{
  if (!order) {
    return std::nullopt;
  }
  return EspmFileList::FromLoadOrder(*order);
}

MpChangeForm RecordReader::Read(simdjson::dom::element& element)
{
  if (!running) {
    return MpChangeForm::JsonToChangeForm(element);
  }

  MpChangeForm changeForm = MpChangeForm::JsonToChangeForm(
    element, &*running, saved ? &*saved : nullptr);
  ++numRead;
  numUnresolved += changeForm.unresolved.size();

  if (changeForm.formVersion < MpChangeForm::kFormVersion) {
    logger->info("ADR-043: {} read in the raw shape, re-based from {} to the "
                 "running order",
                 changeForm.formDesc.ToString(),
                 saved ? "the saved order" : "the running order (none saved)");
    migrated.push_back(changeForm);
  }
  return changeForm;
}

void RecordReader::Commit(Database& database)
{
  if (!running) {
    return;
  }

  const size_t numMigrated = migrated.size();
  if (!migrated.empty()) {
    database.Upsert(std::move(migrated));
    migrated.clear();
  }
  database.UpsertLoadOrder(running->ToLoadOrder());

  logger->info("ADR-043: {} records read under the running order of {} "
               "files; {} in the raw shape re-based and written back as "
               "version {}; {} stored forms name a plugin the order lacks "
               "and are kept",
               numRead, running->size(), numMigrated,
               MpChangeForm::kFormVersion, numUnresolved);
}
