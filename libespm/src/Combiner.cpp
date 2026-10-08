#include "libespm/Combiner.h"
#include "libespm/Browser.h"
#include "libespm/Convert.h"
#include "libespm/Records.h"
#include "libespm/Utils.h"
#include "libespm/espm.h"
#include <array>
#include <cctype>
#include <fmt/format.h>
#include <string>
#include <vector>

namespace espm {

namespace {
// the TES4 record flag that makes a plugin light (CommonLibSSE-NG
// include/RE/T/TESFile.h:48, RecordFlag::kSmallFile)
constexpr uint32_t kSmallFileFlag = 1u << 9;
}

Combiner::Combiner()
  : pImpl(nullptr)
{
  pImpl = std::make_unique<CombineBrowser::Impl>();
}

void espm::Combiner::AddSource(Browser* src, const char* fileName) noexcept
{
  if (pImpl->numSources >= std::size(pImpl->sources)) {
    ++pImpl->numSources;
    return;
  }
  pImpl->sources[pImpl->numSources++] = { src, fileName, nullptr };
}

std::unique_ptr<espm::CombineBrowser> Combiner::Combine()
{
  if (pImpl->numSources > std::size(pImpl->sources)) {
    throw CombineError("too many sources");
  }

  // The engine numbers full and light plugins apart, each in load order
  // (thuum docs/verbs/light-plugins.md): a file's place in the combined
  // numbering is its index among the files of its kind
  std::vector<IdMapping::File> keys(pImpl->numSources);
  uint16_t numFull = 0;
  uint16_t numLight = 0;
  for (size_t i = 0; i < pImpl->numSources; ++i) {
    auto& src = pImpl->sources[i];
    if (!src.br) {
      throw CombineError("nullptr source with index " + std::to_string(i));
    }
    const auto tes4 = Convert<TES4>(src.br->LookupById(0));
    if (!tes4) {
      throw CombineError(src.fileName + " doesn't have TES4 record");
    }
    std::string extension = src.fileName.size() >= 4
      ? src.fileName.substr(src.fileName.size() - 4)
      : std::string();
    for (auto& c : extension) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    src.light =
      (tes4->GetFlags() & kSmallFileFlag) != 0 || extension == ".esl";
    if (src.light) {
      if (numLight > 0x0fff) {
        throw CombineError("more than 4096 light plugins");
      }
      keys[i] = { true, numLight++ };
    } else {
      if (numFull > 0xfd) {
        throw CombineError("more than 254 full plugins");
      }
      keys[i] = { false, numFull++ };
    }
  }

  for (size_t i = 0; i < pImpl->numSources; ++i) {
    auto& src = pImpl->sources[i];
    const auto tes4 = Convert<TES4>(src.br->LookupById(0));
    espm::CompressedFieldsCache dummyCache;
    const auto masters = tes4->GetData(dummyCache).masters;

    // inside a file an id's top byte indexes its master list, itself last
    auto toComb = std::make_unique<IdMapping>();
    auto toRaw = std::make_unique<IdMapping>();
    size_t m = 0;
    for (m = 0; m < masters.size(); ++m) {
      const int globalIdx = pImpl->GetFileIndex(masters[m]);
      if (globalIdx == -1) {
        throw CombineError(src.fileName + " has unresolved dependency (" +
                           masters[m] + ")");
      }
      const IdMapping::File raw{ false, static_cast<uint16_t>(m) };
      toComb->Set(raw, keys[globalIdx]);
      toRaw->Set(keys[globalIdx], raw);
    }
    const IdMapping::File raw{ false, static_cast<uint16_t>(m) };
    toComb->Set(raw, keys[i]);
    toRaw->Set(keys[i], raw);
    src.toComb = std::move(toComb);
    src.toRaw = std::move(toRaw);
  }

  std::unique_ptr<espm::CombineBrowser> res(new CombineBrowser);
  res->pImpl = pImpl;
  return res;
}

Combiner::~Combiner() = default;

}
