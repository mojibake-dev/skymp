#pragma comment(lib, "shell32.lib")
#include "LoadGame.h"
#include "NullPointerException.h"
#include "PapyrusTESModPlatform.h"
#include "savefile/SFChangeFormNPC.h"
#include "savefile/SFReader.h"
#include "savefile/SFSeekerOfDifferences.h"
#include "savefile/SFWriter.h"

#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

CMRC_DECLARE(skyrim_plugin_resources);

constexpr auto g_saveFilePrefix = "TESMODPLATFORM-";

namespace {
// FormIdInSave's fall-throughs at one login: PrepareSaveFile starts a login,
// each kind's first id gets a line, and Run ends the login with the counts
// when a kind fell through more than once (ADR-073 decision 7). loadGame runs
// on the JS thread, one login at a time.
enum FallThroughKind
{
  kLight,
  kNoPlugin,
  kNotListed,
  kFallThroughKinds
};
constexpr const char* kFallThroughNames[kFallThroughKinds] = { "light",
                                                               "no plugin",
                                                               "not listed" };
int g_fallThroughs[kFallThroughKinds] = {};

uint32_t FellThrough(FallThroughKind kind, uint32_t formId,
                     spdlog::level::level_enum level, const std::string& why)
{
  if (g_fallThroughs[kind]++ == 0) {
    spdlog::log(level,
                "[loadGame] FormIdInSave fell through ({}): {:08X}; written "
                "as it is ({})",
                kFallThroughNames[kind], formId, why);
  }
  return formId;
}

void StartFallThroughs()
{
  std::fill(std::begin(g_fallThroughs), std::end(g_fallThroughs), 0);
}

void EndFallThroughs()
{
  if (std::none_of(std::begin(g_fallThroughs), std::end(g_fallThroughs),
                   [](int n) { return n > 1; })) {
    return;
  }
  std::string counts;
  for (int k = 0; k < kFallThroughKinds; ++k) {
    counts += fmt::format("{}{} {}", k ? ", " : "", kFallThroughNames[k],
                          g_fallThroughs[k]);
  }
  spdlog::info("[loadGame] FormIdInSave fell through at this login: {}",
               counts);
}
}

class LoadGameEventSink : public RE::BSTEventSink<RE::TESLoadGameEvent>
{
public:
  LoadGameEventSink()
  {
    auto holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder) {
      throw NullPointerException("holder");
    }

    holder->AddEventSink(
      dynamic_cast<RE::BSTEventSink<RE::TESLoadGameEvent>*>(this));
  }

  RE::BSEventNotifyControl ProcessEvent(
    const RE::TESLoadGameEvent* event,
    RE::BSTEventSource<RE::TESLoadGameEvent>* eventSource) override
  {
    std::thread([] {
      // A way to wait 5 seconds game time
      for (int i = 0; i < 50; ++i) {
        auto n = TESModPlatform::GetNumPapyrusUpdates();
        while (n == TESModPlatform::GetNumPapyrusUpdates()) {
          Sleep(20);
        }
        Sleep(80);
      }

      // Allow MoveRefrToPosition again
      TESModPlatform::BlockMoveRefrToPosition(false);

      // Removes our temporary save files
      try {
        std::filesystem::path path = LoadGame::GetPathToMyDocuments() +
          L"\\My Games\\Skyrim Special Edition\\Saves\\";

        for (auto& file : std::filesystem::directory_iterator(path)) {
          if (file.path().filename().generic_string().find(g_saveFilePrefix) !=
              std::string::npos)
            try {
              std::filesystem::remove(file);
            } catch (...) {
              // I have no idea how to handle these exceptions properly
            }
        }
      } catch (...) {
      }
    }).detach();
    return RE::BSEventNotifyControl::kContinue;
  }
};

extern bool g_allowHideMainMenu;

std::shared_ptr<SaveFile_::SaveFile> LoadGame::PrepareSaveFile(
  const char* pathInAssets, const std::vector<std::string>* loadOrder)
{
  cmrc::file file;
  try {
    file = cmrc::skyrim_plugin_resources::get_filesystem().open(pathInAssets);
  } catch (std::exception& e) {
    auto dir =
      cmrc::skyrim_plugin_resources::get_filesystem().iterate_directory("");
    std::stringstream ss;
    ss << e.what() << std::endl << std::endl;
    ss << "Root directory contents is: " << std::endl;
    for (auto entry : dir) {
      ss << entry.filename() << std::endl;
    }
    throw std::runtime_error(ss.str());
  }
  auto save =
    SaveFile_::Reader((uint8_t*)(file.begin()), file.size()).GetStructure();
  StartFallThroughs();
  ModifyPluginInfo(save, loadOrder);
  return save;
}

void LoadGame::Run(std::shared_ptr<SaveFile_::SaveFile> save,
                   const std::array<float, 3>& pos,
                   const std::array<float, 3>& angle, uint32_t cellOrWorld,
                   Time* time, SaveFile_::Weather* _weather,
                   SaveFile_::ChangeFormNPC_* changeFormNPC)
{
  if (!save) {
    throw std::runtime_error("Bad SaveFile");
  }

  ModifySaveTime(save, time);
  ModifySaveWeather(save, _weather);
  ModifyPlayerFormNPC(save, changeFormNPC);
  ModifyEssStructure(save, pos, angle, cellOrWorld);
  EndFallThroughs();

  auto name = g_saveFilePrefix + GenerateGuid();
  if (!SaveFile_::Writer(save).CreateSaveFile(GetSaveFullPath(name))) {
    throw std::runtime_error("CreateSaveFile failed");
  }

  TESModPlatform::BlockMoveRefrToPosition(true);
  static LoadGameEventSink g_sink;

  if (auto saveLoadManager = RE::BGSSaveLoadManager::GetSingleton()) {
    return saveLoadManager->Load(name.data());
  } else {
    throw NullPointerException("saveLoadManager");
  }
}

fs::path LoadGame::GetSaveFullPath(const std::string& name)
{
  return GetPathToMyDocuments() +
    L"\\My Games\\Skyrim Special Edition\\Saves\\" + StringToWstring(name) +
    L".ess";
}

std::wstring LoadGame::GetPathToMyDocuments()
{
  PWSTR ppszPath;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &ppszPath);
  std::wstring myPath;
  if (SUCCEEDED(hr)) {
    myPath = ppszPath;
  }
  CoTaskMemFree(ppszPath);
  return myPath;
}

// thuum: the template's plugin list keeps its entries, and the seat's other
// plugins go after them (SaveFile::MergePluginInfo); the game matches the list
// to its own plugins by name as it loads (HYPOTHESIS: Ghidra on sky-re,
// 1.6.1170, 0x140606df0 and 0x1406057e0, until a lab run). This wrote the
// seat's load order over the list by position (dataHandler->files, then the
// client's list from ModifyLoadOrder), and no form id in the template moved
// with it: the template lists 02 Dragonborn, 03 Dawnguard and 04 HearthFires
// and the deployed order 02 Dawnguard, 03 HearthFires and 04 Dragonborn, so
// VC01's DLC1VampireChange (0x0300283B in the template) named HearthFires.esm,
// which has no such form, and reached the scripts as None (playtest 52's
// E_SCRIPTCALL_UNNAMED Actor.RemoveSpell 0).
//
// The plugins appended are the game's own loaded ones (TESDataHandler's
// compiled files, GetLoadedMods), the list the game compares the save's with
// and FormIdInSave looks plugins up in, so one source serves both. A caller's
// load order (the client sends Game.getModName's, the same files:
// CallNativeApi.cpp) is only compared with it, by index and in any case as
// the game compares names, with one line where it differs.
//
// A template plugin the seat lacks is logged at warn: on Skyrim Platform's
// Load the game goes on with that plugin's forms resolving to none
// (HYPOTHESIS: Ghidra on sky-re, 1.6.1170: Load's last argument, true,
// reaches 0x140605f90 as its param_3 through LoadRequest's byte +0x24 at
// 0x140613dfc, and 0x140605f90 stops at a missing plugin only when param_3 is
// 0; 0x140606df0 keeps a null file for it, which 0x1406057e0 reads as form 0).
void LoadGame::ModifyPluginInfo(std::shared_ptr<SaveFile_::SaveFile>& save,
                                const std::vector<std::string>* loadOrder)
{
  auto dataHandler = RE::TESDataHandler::GetSingleton();
  if (!dataHandler) {
    throw NullPointerException("dataHandler");
  }
  std::vector<std::string> loaded;
  const auto files = dataHandler->GetLoadedMods();
  for (uint32_t i = 0; i < dataHandler->GetLoadedModCount(); ++i) {
    // the game checks each entry before it reads a name (0x140a50d70)
    if (files[i]) {
      loaded.push_back(files[i]->fileName);
    }
  }

  if (loadOrder) {
    const size_t n = std::max(loadOrder->size(), loaded.size());
    for (size_t i = 0; i < n; ++i) {
      const bool both = i < loadOrder->size() && i < loaded.size();
      if (!both || _stricmp((*loadOrder)[i].c_str(), loaded[i].c_str()) != 0) {
        spdlog::info("[loadGame] the caller's load order differs from the "
                     "game's at {} ({} vs {}); the game's is used",
                     i, i < loadOrder->size() ? (*loadOrder)[i] : "(none)",
                     i < loaded.size() ? loaded[i] : "(none)");
        break;
      }
    }
  }

  const auto missing = save->MergePluginInfo(loaded);

  const auto& names = save->pluginInfo.pluginsName;
  std::string written;
  for (size_t i = 0; i < names.size(); ++i) {
    written += fmt::format("{}{:02X} {}", i ? ", " : "", i, names[i]);
  }
  std::string lacks;
  for (auto& name : missing) {
    lacks += (lacks.empty() ? "" : ", ") + name;
  }
  spdlog::log(missing.empty() ? spdlog::level::info : spdlog::level::warn,
              "[loadGame] the login save's plugin list ({}); template plugins "
              "this seat lacks: {}; {}",
              names.size(), lacks.empty() ? "none" : lacks, written);
}

// A form id of this game, its plugin by compileIndex, goes into the save
// under the index the save's list gives that plugin's name. Skyrim.esm's (00)
// and a created form (FF) read the same in both. A light plugin's (FE) has no
// index in a list of form version 74, which carries no light list
// (HYPOTHESIS: Ghidra on sky-re, 1.6.1170, 0x140606df0 and 0x1406057e0, until
// a lab run), and is written as it is. So is an id under an index no loaded
// plugin has: it names nothing here, and nothing in the save while the save's
// list is no longer than the game's (0x1406057e0 gives 0 past the count); a
// template plugin the seat lacks makes the save's list longer, and then such
// an id names an appended plugin. Every loaded plugin is in the save's list
// once ModifyPluginInfo has merged the game's own, so "not listed" is an
// alarm that should never fire.
uint32_t LoadGame::FormIdInSave(const SaveFile_::SaveFile& save,
                                uint32_t formId)
{
  const uint32_t plugin = formId >> 24;
  if (plugin == 0x00 || plugin == 0xff) {
    return formId;
  }
  if (plugin == 0xfe) {
    return FellThrough(kLight, formId, spdlog::level::info,
                       fmt::format("the login save, form version {}, "
                                   "carries no light list",
                                   static_cast<int>(save.formVersion)));
  }

  auto dataHandler = RE::TESDataHandler::GetSingleton();
  if (!dataHandler) {
    throw NullPointerException("dataHandler");
  }
  const RE::TESFile* file =
    dataHandler->LookupLoadedModByIndex(static_cast<uint8_t>(plugin));
  if (!file) {
    return FellThrough(
      kNoPlugin, formId, spdlog::level::info,
      fmt::format("no loaded plugin has index {:02X}", plugin));
  }
  const auto inSave = save.FormIdUnderPlugin(file->fileName, formId);
  if (!inSave) {
    return FellThrough(kNotListed, formId, spdlog::level::warn,
                       fmt::format("{} is not in the login save's plugin list",
                                   file->fileName));
  }
  return *inSave;
}

void LoadGame::ModifySaveTime(std::shared_ptr<SaveFile_::SaveFile>& save,
                              LoadGame::Time* time)
{
  if (!time) {
    return;
  }

  if (!time->IsSet()) {
    throw std::runtime_error("Time data is not filled");
  }

  SaveFile_::RefID gameHourID = 0x38;

  auto index = save->FindIndexInFormIdArray(0x38);
  if (index >= 0) {
    gameHourID = SaveFile_::RefID(static_cast<uint32_t>(index));
  }

  auto var = save->GetGlobalvariableByRefID(gameHourID);
  if (!var) {
    throw std::runtime_error("Global Varible not found");
  }

  var->value =
    time->GetHours() + time->GetMinutes() / 60.0 + time->GetSeconds() / 3600.0;
}

void LoadGame::ModifySaveWeather(std::shared_ptr<SaveFile_::SaveFile>& save,
                                 SaveFile_::Weather* _weather)
{
  if (!_weather) {
    return;
  }

  SaveFile_::GlobalData& gData =
    save->globalDataTable1[SaveFile_::SaveFile::WEATHER_INDEX];

  if (gData.type != SaveFile_::SaveFile::WEATHER_INDEX) {
    throw std::runtime_error("Wrong weather index");
  }

  SaveFile_::Weather* weather =
    reinterpret_cast<SaveFile_::Weather*>(gData.data.get());

  if (!weather) {
    throw NullPointerException("weather");
  }

  weather->climate = _weather->climate;
  weather->weather = _weather->weather;
  weather->regnWeather = _weather->regnWeather;
  weather->weatherPct = _weather->weatherPct;
}

void LoadGame::ModifyPlayerFormNPC(std::shared_ptr<SaveFile_::SaveFile> save,
                                   SaveFile_::ChangeFormNPC_* changeFormNPC)
{
  using namespace SaveFile_;
  if (!changeFormNPC) {
    return;
  }

  auto form = save->GetChangeFormByRefID(RefID(RefID::PlayerBase),
                                         uint8_t(ChangeForm::Type::NPC));

  if (form) {
    auto newForm = changeFormNPC->ToBinary();
    FillChangeForm(save, form, newForm);
  }
}

void LoadGame::FillChangeForm(
  std::shared_ptr<SaveFile_::SaveFile> save, SaveFile_::ChangeForm* form,
  std::pair<uint32_t, std::vector<uint8_t>>& newValues)
{

  save->fileLocationTable.formIDArrayCountOffset -= form->length1;
  save->fileLocationTable.formIDArrayCountOffset += newValues.second.size();

  save->fileLocationTable.unknownTable3Offset -= form->length1;
  save->fileLocationTable.unknownTable3Offset += newValues.second.size();

  save->fileLocationTable.globalDataTable3Offset -= form->length1;
  save->fileLocationTable.globalDataTable3Offset += newValues.second.size();

  form->length2 = 0;
  form->length1 = newValues.second.size();
  form->data = newValues.second;
  form->changeFlags = newValues.first;
}

void LoadGame::ModifyEssStructure(std::shared_ptr<SaveFile_::SaveFile> save,
                                  std::array<float, 3> pos,
                                  std::array<float, 3> angle,
                                  uint32_t cellOrWorld)
{
  auto playerLoc = FindSectionWithPlayerLocation(save);
  if (!playerLoc) {
    throw std::runtime_error("Couldn't find PlayerLocation in the save file");
  }

  auto worldRefId =
    SaveFile_::RefID::CreateRefId(*save, FormIdInSave(*save, cellOrWorld));
  *playerLoc = CreatePlayerLocation(pos, worldRefId);

  auto player = std::find_if(
    save->changeForms.begin(), save->changeForms.end(),
    [](auto& changeForm) { return changeForm.formID.IsPlayerID(); });
  if (player == save->changeForms.end()) {
    throw std::runtime_error("Unable to find Player's change form");
  }
  bool isCompressed = player->length2 > 0;
  if (!isCompressed) {
    throw std::runtime_error("Player's ChangeForm must be compressed");
  }

  auto uncompressed = Decompress(*player);
  EditChangeForm(uncompressed, pos, angle, worldRefId);
  auto compressed = Compress(uncompressed);
  WriteChangeForm(save, *player, compressed, uncompressed.size());
}

SaveFile_::PlayerLocation* LoadGame::FindSectionWithPlayerLocation(
  std::shared_ptr<SaveFile_::SaveFile> save)
{
  auto& c = save->globalDataTable1;
  auto it = std::find_if(
    c.begin(), c.end(), [](const SaveFile_::GlobalData& globalData) {
      return globalData.type == SaveFile_::PlayerLocation::GlobalDataType;
    });
  if (it == c.end()) {
    return nullptr;
  }
  return static_cast<SaveFile_::PlayerLocation*>(it->data.get());
}

SaveFile_::PlayerLocation LoadGame::CreatePlayerLocation(
  const std::array<float, 3>& pos, const SaveFile_::RefID& world)
{
  SaveFile_::PlayerLocation r;
  r.nextObjectId = 0xFF0014FE;
  r.worldspace1 = world;
  r.coorX = static_cast<int>(pos[0]) / 4096;
  r.coorY = static_cast<int>(pos[1]) / 4096;
  r.worldspace2 = world;
  r.posX = pos[0];
  r.posY = pos[1];
  r.posZ = pos[2];
  r.unknown = 0;
  return r;
}

std::vector<uint8_t> LoadGame::Decompress(
  const SaveFile_::ChangeForm& changeForm)
{
  const size_t lengthCompressed = changeForm.length1;
  const size_t lengthUncompressed = changeForm.length2;
  auto uncompressed = (std::vector<uint8_t>(lengthUncompressed));
  const auto compressed = changeForm.data.data();
  SaveFile_::SeekerOfDifferences::ZlibDecompress(
    compressed, lengthCompressed, uncompressed.data(), lengthUncompressed);
  return uncompressed;
}

void LoadGame::EditChangeForm(std::vector<uint8_t>& data,
                              const std::array<float, 3>& pos,
                              const std::array<float, 3>& angle,
                              const SaveFile_::RefID& world)
{
  auto d = data.data();
  *reinterpret_cast<SaveFile_::RefID*>(d + 0) = world;

  auto& changeFormPos = *reinterpret_cast<std::array<float, 3>*>(d + 3);
  changeFormPos = pos;

  float* changeFormAngle = reinterpret_cast<float*>(d + 15);
  for (int i = 0; i < 3; ++i) {
    changeFormAngle[i] = angle[i] / 180.f * acos(-1);
  }
}

std::vector<uint8_t> LoadGame::Compress(
  const std::vector<uint8_t>& uncompressed)
{
  size_t newCompressedSizeMax = uncompressed.size();
  std::vector<uint8_t> newCompressed(newCompressedSizeMax, 0);
  const auto newCompressedSize = SaveFile_::SeekerOfDifferences::ZlibCompress(
    uncompressed.data(), uncompressed.size(), newCompressed.data(),
    newCompressedSizeMax);
  newCompressed.resize(newCompressedSize);
  return newCompressed;
}

void LoadGame::WriteChangeForm(std::shared_ptr<SaveFile_::SaveFile> save,
                               SaveFile_::ChangeForm& changeForm,
                               const std::vector<uint8_t>& compressed,
                               size_t uncompressedSize)
{
  auto previousSize = changeForm.length1;

  changeForm.length1 = compressed.size();
  changeForm.length2 = uncompressedSize;
  changeForm.data.resize(changeForm.length1);
  std::copy(compressed.begin(), compressed.end(), changeForm.data.begin());

  // fix offsets
  const auto diff = static_cast<int64_t>(previousSize) -
    static_cast<int64_t>(compressed.size());
  save->fileLocationTable.formIDArrayCountOffset -= diff;
  save->fileLocationTable.unknownTable3Offset -= diff;
  save->fileLocationTable.globalDataTable3Offset -= diff;
}

std::wstring LoadGame::StringToWstring(const std::string& s)
{
  std::wstring ws(s.size(), L' ');
  auto n = std::mbstowcs(&ws[0], s.c_str(), s.size());
  ws.resize(n);
  return ws;
}

std::string LoadGame::GenerateGuid()
{
  GUID guid;
  if (CoCreateGuid(&guid) != S_OK) {
    throw std::runtime_error("CoCreateGuid failed");
  }

  char name[37] = { 0 }; // Size adjusted for GUID string
  sprintf_s(
    name,
    "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
    guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
    guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
    guid.Data4[7]);
  return name;
}
