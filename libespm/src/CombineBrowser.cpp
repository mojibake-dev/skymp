#include "libespm/CombineBrowser.h"
#include "libespm/Browser.h"
#include "libespm/RecordHeader.h"
#include "libespm/Utils.h"
#include <array>
#include <cctype>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <unordered_set>

namespace espm {

namespace {
// A plugin's name as the engine compares it: without case. A master list
// names its masters as their authors typed them (USSEP: ccbgssse001-fish.esm)
// and the load order names the files as the disk has them
// (ccBGSSSE001-Fish.esm); Windows never tells them apart, Linux does
// (thuum, the essentials layer's first load, 2026-10-19: "unofficial skyrim
// special edition patch.esp has unresolved dependency (ccbgssse001-fish.esm)"
// with the file fifth in the order)
bool SameFileName(const std::string& a, const char* b) noexcept
{
  const size_t n = std::strlen(b);
  if (a.size() != n) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}
}

int32_t CombineBrowser::Impl::GetFileIndex(const char* fileName) const noexcept
{
  // returns index of sources array or -1 if not found
  if (fileName[0] != '\0') {
    for (size_t i = 0; i < sources.size(); ++i) {
      if (SameFileName(sources[i].fileName, fileName)) {
        return i;
      }
    }
  }
  return -1;
}

LookupResult CombineBrowser::LookupById(uint32_t combFormId) const noexcept
{
  // Otherwise, we'll find a TES4 record in Skyrim.esm which is not relevant
  // for CombineBrowser use cases
  if (combFormId == 0) {
    return LookupResult();
  }

  const RecordHeader* resRec = nullptr;
  uint8_t resFileIdx = 0;
  for (size_t i = 0; i < pImpl->numSources; ++i) {
    auto& src = pImpl->sources[i];
    const uint32_t rawFormId = utils::GetMappedId(combFormId, *src.toRaw);
    if (rawFormId >= 0xff000000)
      continue;
    auto rec = src.br->LookupById(rawFormId);
    if (rec) {
      resRec = rec;
      resFileIdx = (uint8_t)i;
    }
  }
  return resRec ? LookupResult(this, resRec, resFileIdx) : LookupResult();
}

std::vector<LookupResult> CombineBrowser::LookupByIdAll(
  uint32_t combFormId) const noexcept
{
  std::vector<LookupResult> res;
  for (size_t i = 0; i < pImpl->numSources; ++i) {
    auto& src = pImpl->sources[i];
    const uint32_t rawFormId = utils::GetMappedId(combFormId, *src.toRaw);
    if (rawFormId >= 0xff000000)
      continue;
    const RecordHeader* rec = src.br->LookupById(rawFormId);
    if (rec) {
      res.push_back({ this, rec, static_cast<uint8_t>(i) });
    }
  }
  return res;
}

std::pair<const RecordHeader**, size_t> CombineBrowser::FindNavMeshes(
  uint32_t worldSpaceId, CellOrGridPos cellOrGridPos) const noexcept
{
  for (size_t i = 0; i < pImpl->numSources; ++i) {
    auto& src = pImpl->sources[i];
    const uint32_t rawFormId = utils::GetMappedId(worldSpaceId, *src.toRaw);
    if (rawFormId >= 0xff000000)
      continue;
    auto p = src.br->FindNavMeshes(worldSpaceId, cellOrGridPos);
    auto front = p.first;
    auto size = p.second;
    if (front && *front) {
      return { front, size };
    }
  }
  return { nullptr, 0 };
}

LookupResult CombineBrowser::FindExteriorCell(uint32_t worldSpaceId,
                                              int16_t gridX,
                                              int16_t gridY) const noexcept
{
  for (size_t i = pImpl->numSources; i-- > 0;) {
    auto& src = pImpl->sources[i];
    const uint32_t rawWorldId = utils::GetMappedId(worldSpaceId, *src.toRaw);
    if (rawWorldId >= 0xff000000) {
      continue;
    }
    if (auto rec = src.br->FindExteriorCell(rawWorldId, gridX, gridY)) {
      return LookupResult(this, rec, static_cast<uint8_t>(i));
    }
  }
  return LookupResult();
}

std::vector<const std::vector<const RecordHeader*>*>
CombineBrowser::GetRecordsByType(const char* type) const
{
  std::vector<const std::vector<const RecordHeader*>*> res;
  for (size_t i = 0; i < pImpl->numSources; ++i) {
    res.push_back(&pImpl->sources[i].br->GetRecordsByType(type));
  }
  return res;
}

std::vector<LookupResult> CombineBrowser::GetDistinctRecordsByType(
  const char* type) const
{
  if (pImpl->numSources == 0) {
    return {};
  }

  std::unordered_set<formId> formSet;
  std::vector<LookupResult> result;
  for (size_t i = pImpl->numSources - 1; i != static_cast<size_t>(-1); --i) {
    const auto& records = pImpl->sources[i].br->GetRecordsByType(type);
    formSet.reserve(records.size());
    result.reserve(records.size());

    for (auto record : records) {
      auto mappedId =
        utils::GetMappedId(record->GetId(), *pImpl->sources[i].toComb);
      if (formSet.insert(mappedId).second) {
        result.push_back(LookupResult(this, record, static_cast<uint8_t>(i)));
      }
    }
  }

  return result;
}

std::vector<const std::vector<const RecordHeader*>*>
CombineBrowser::GetRecordsAtPos(uint32_t cellOrWorld, int16_t cellX,
                                int16_t cellY) const
{
  std::vector<const std::vector<const RecordHeader*>*> res;
  for (size_t i = 0; i < pImpl->numSources; ++i) {
    res.push_back(
      &pImpl->sources[i].br->GetRecordsAtPos(cellOrWorld, cellX, cellY));
  }
  return res;
}

const IdMapping* CombineBrowser::GetCombMapping(
  size_t fileIndex) const noexcept
{
  if (fileIndex >= pImpl->numSources) {
    return nullptr;
  }
  return pImpl->sources[fileIndex].toComb.get();
}

const IdMapping* CombineBrowser::GetRawMapping(size_t fileIndex) const noexcept
{
  if (fileIndex >= pImpl->numSources) {
    return nullptr;
  }
  return pImpl->sources[fileIndex].toRaw.get();
}

bool CombineBrowser::IsLight(size_t fileIndex) const noexcept
{
  return fileIndex < pImpl->numSources && pImpl->sources[fileIndex].light;
}

CompressedFieldsCache& CombineBrowser::GetCache() const noexcept
{
  return pImpl->cache;
}

const GroupStack& CombineBrowser::GetParentGroupsEnsured(
  const RecordHeader* rec) const
{
  for (size_t i = 0; i < pImpl->numSources; ++i) {
    const auto result = pImpl->sources[i].br->GetParentGroupsOptional(rec);
    if (result) {
      return *result;
    }
  }
  throw std::runtime_error(fmt::format(
    "espm::CombineBrowser: no browsers know record id={:#x}", rec->GetId()));
}

const std::vector<const void*>& CombineBrowser::GetSubsEnsured(
  const GroupHeader* group) const
{
  for (size_t i = 0; i < pImpl->numSources; ++i) {
    const auto result = pImpl->sources[i].br->GetSubsOptional(group);
    if (result) {
      return *result;
    }
  }
  throw std::runtime_error(
    "espm::CombineBrowser: no browsers know requested group");
}

}
