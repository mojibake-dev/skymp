#include "ZipDatabase.h"

#include "LoadOrderFile.h"
#include "RecordReader.h"
#include "libzippp/libzippp.h"
#include <FileUtils.h>
#include <MappedBuffer.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <save_storages/AsyncSaveStorage.h>
#include <unordered_set>

struct ZipDatabase::Impl
{
  Impl(std::string filePath_, std::shared_ptr<spdlog::logger> logger_)
    : filePath(std::move(filePath_))
    , logger(std::move(logger_))
  {
  }

  std::string filePath;
  std::shared_ptr<spdlog::logger> logger;
};

ZipDatabase::ZipDatabase(std::string filePath_,
                         std::shared_ptr<spdlog::logger> logger_)
  : pImpl(std::make_shared<Impl>(std::move(filePath_), std::move(logger_)))
{
}

ZipDatabase::~ZipDatabase() = default;

std::vector<std::optional<MpChangeForm>>&& ZipDatabase::UpsertImpl(
  std::vector<std::optional<MpChangeForm>>&& changeForms,
  size_t& outNumUpserted)
{
  try {
    auto filePathAbsolute =
      std::filesystem::absolute(pImpl->filePath).string();

    libzippp::ZipArchive archive(filePathAbsolute.data());

    archive.open(libzippp::ZipArchive::Write);

    std::vector<std::unique_ptr<std::string>> buffers;

    // ADR-043: written against the running order when there is one
    const auto files = RecordReader::Files(GetLoadOrder());

    for (auto& changeForm : changeForms) {
      if (changeForm == std::nullopt) {
        continue;
      }

      // Data to be added or updated
      std::string data =
        MpChangeForm::ToJson(*changeForm, files ? &*files : nullptr).dump(2);
      std::string fileName = changeForm->formDesc.ToString('_') + ".json";
      std::string filePath = fileName;

      buffers.push_back(std::make_unique<std::string>(data));

      // Add new file or replace existing one
      archive.addData(filePath, buffers.back()->data(),
                      buffers.back()->size());
    }

    archive.close();

    outNumUpserted = changeForms.size();

    return std::move(changeForms);
  } catch (std::exception& e) {
    throw Viet::AsyncSaveStorage<
      MpChangeForm, FormDesc,
      std::vector<FormDesc>>::UpsertFailedException(std::move(changeForms),
                                                    e.what());
  }
}

void ZipDatabase::Iterate(const IterateCallback& iterateCallback,
                          std::optional<std::vector<FormDesc>> filter)
{
  try {

    auto filePathAbsolute =
      std::filesystem::absolute(pImpl->filePath).string();

    std::optional<std::unordered_set<std::string>> filterSet;
    if (filter) {
      std::unordered_set<std::string>& value = filterSet.emplace();
      for (const auto& desc : *filter) {
        value.insert(desc.ToString());
      }
    }

    // ADR-043: records read under the running order, a raw-shape world
    // re-based from the order it saved and written back once the archive is
    // closed for reading
    RecordReader reader(GetLoadOrder(), LoadLoadOrder(), pImpl->logger);

    {
      libzippp::ZipArchive archive(filePathAbsolute.data());

      archive.open(libzippp::ZipArchive::ReadOnly);

      std::vector<libzippp::ZipEntry> entries = archive.getEntries();
      std::vector<libzippp::ZipEntry>::iterator it;

      simdjson::dom::parser p;

      for (it = entries.begin(); it != entries.end(); ++it) {
        libzippp::ZipEntry entry = *it;
        std::string name = entry.getName();
        int size = entry.getSize();

        // the saved order is no record
        if (name == LoadOrderFile::kName) {
          continue;
        }

        std::string textData = entry.readAsText();

        try {
          auto result = p.parse(textData).value();
          auto changeForm = reader.Read(result);

          if (filterSet) {
            if (filterSet->find(changeForm.formDesc.ToString()) ==
                filterSet->end()) {
              continue;
            }
          }

          iterateCallback(changeForm);
        } catch (std::exception& e) {
          pImpl->logger->error("Parsing or loading of {} failed with {}", name,
                               e.what());
        }
      }

      archive.close();
    }

    reader.Commit(*this);

  } catch (std::exception& e) {
    throw Viet::AsyncSaveStorage<
      MpChangeForm, FormDesc,
      std::vector<FormDesc>>::IterateFailedException(std::move(filter),
                                                     e.what());
  }
}

void ZipDatabase::UpsertLoadOrder(const LoadOrder& order)
{
  auto filePathAbsolute = std::filesystem::absolute(pImpl->filePath).string();

  libzippp::ZipArchive archive(filePathAbsolute.data());
  archive.open(libzippp::ZipArchive::Write);

  // the buffer must outlive close(), which writes the archive
  const std::string data = LoadOrderFile::ToJson(order).dump(2);
  archive.addData(LoadOrderFile::kName, data.data(), data.size());

  archive.close();
}

std::optional<ZipDatabase::LoadOrder> ZipDatabase::LoadLoadOrder()
{
  auto filePathAbsolute = std::filesystem::absolute(pImpl->filePath).string();

  libzippp::ZipArchive archive(filePathAbsolute.data());
  archive.open(libzippp::ZipArchive::ReadOnly);

  libzippp::ZipEntry entry = archive.getEntry(LoadOrderFile::kName);
  if (entry.isNull()) {
    archive.close();
    return std::nullopt;
  }

  const std::string text = entry.readAsText();
  archive.close();
  return LoadOrderFile::FromJson(nlohmann::json::parse(text));
}
