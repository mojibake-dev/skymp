#pragma comment(lib, "shell32.lib")
#include "LoadGame.h"
#include "NullPointerException.h"
#include "PapyrusTESModPlatform.h"
#include "savefile/SFChangeFormNPC.h"
#include "savefile/SFReader.h"
#include "savefile/SFSeekerOfDifferences.h"
#include "savefile/SFWriter.h"

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

CMRC_DECLARE(skyrim_plugin_resources);

constexpr auto g_saveFilePrefix = "TESMODPLATFORM-";

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
// to its own plugins by name as it loads. This wrote the seat's load order
// over the list by position (dataHandler->files, then the client's list from
// ModifyLoadOrder), and no form id in the template moved with it: the
// template lists 02 Dragonborn, 03 Dawnguard and 04 HearthFires and the
// deployed order 02 Dawnguard, 03 HearthFires and 04 Dragonborn, so VC01's
// DLC1VampireChange (0x0300283B in the template) named HearthFires.esm, which
// has no such form, and reached the scripts as None (playtest 52's
// E_SCRIPTCALL_UNNAMED Actor.RemoveSpell 0). The list is the caller's load
// order (the client sends Game.getModName's), else the game's loaded plugins.
void LoadGame::ModifyPluginInfo(std::shared_ptr<SaveFile_::SaveFile>& save,
                                const std::vector<std::string>* loadOrder)
{
  std::vector<std::string> loaded;
  if (loadOrder) {
    loaded = *loadOrder;
  } else {
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
      throw NullPointerException("dataHandler");
    }
    const auto files = dataHandler->GetLoadedMods();
    for (uint32_t i = 0; i < dataHandler->GetLoadedModCount(); ++i) {
      loaded.push_back(files[i]->fileName);
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
  spdlog::info("[loadGame] the login save's plugin list as written ({}): {}; "
               "template plugins this seat lacks: {}",
               names.size(), written, lacks.empty() ? "none" : lacks);
}

// A form id of this game, its plugin by compileIndex, goes into the save
// under the index the save's list gives that plugin's name. Skyrim.esm's (00)
// and a created form (FF) read the same in both. A light plugin's (FE) has no
// index in a list of form version 74, which carries no light list, and a form
// whose plugin the list does not name has none either: each is written as it
// is, with a line.
uint32_t LoadGame::FormIdInSave(const SaveFile_::SaveFile& save,
                                uint32_t formId)
{
  const uint32_t plugin = formId >> 24;
  if (plugin == 0x00 || plugin == 0xff) {
    return formId;
  }
  if (plugin == 0xfe) {
    spdlog::info("[loadGame] {:08X} is a light plugin's form, and the login "
                 "save (form version {}) carries no light list; written as it "
                 "is",
                 formId, static_cast<int>(save.formVersion));
    return formId;
  }

  auto dataHandler = RE::TESDataHandler::GetSingleton();
  if (!dataHandler) {
    throw NullPointerException("dataHandler");
  }
  const RE::TESFile* file =
    dataHandler->LookupLoadedModByIndex(static_cast<uint8_t>(plugin));
  if (!file) {
    spdlog::info("[loadGame] {:08X}: no loaded plugin has index {:02X}; "
                 "written as it is",
                 formId, plugin);
    return formId;
  }
  const auto inSave = save.FormIdUnderPlugin(file->fileName, formId);
  if (!inSave) {
    spdlog::info("[loadGame] {:08X}: {} is not in the login save's plugin "
                 "list; written as it is",
                 formId, file->fileName);
    return formId;
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
