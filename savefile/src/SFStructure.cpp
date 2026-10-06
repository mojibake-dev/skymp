#include "savefile/SFStructure.h"

#include <cstring>
#include <stdexcept>

SaveFile_::RefID SaveFile_::RefID::CreateRefId(SaveFile& parentSaveFile,
                                               uint32_t formId)
{
  // A form by its index in the save's formIDArray (RefID type 0; uesp.net:
  // the index starts at 1, 0 meaning form 0). An entry already there is
  // reused; a new one goes at the end, and the tables after the array move
  // by its four bytes. (It used to copy countWas bytes of the old array
  // instead of countWas entries, and leaked the copy; nothing called it.)
  uint32_t index = 0;
  if (const int64_t found = parentSaveFile.FindIndexInFormIdArray(formId);
      found >= 0) {
    index = static_cast<uint32_t>(found) + 1;
  } else {
    parentSaveFile.formIDArray.push_back(formId);
    parentSaveFile.formIDArrayCount =
      static_cast<uint32_t>(parentSaveFile.formIDArray.size());
    parentSaveFile.fileLocationTable.unknownTable3Offset += 4;
    index = parentSaveFile.formIDArrayCount;
  }

  // 255 => 00 00 FF
  // 256 => 00 01 00
  // 65536 => error
  if (index >= 65536)
    throw std::runtime_error("too many elements was in FormIDArray (" +
                             std::to_string(index - 1) + ")");
  RefID res;
  res.byte0 = 0;
  res.byte1 = (index / 256) % 256;
  res.byte2 = index % 256;
  return res;
}

SaveFile_::ChangeForm* SaveFile_::SaveFile::GetChangeFormByRefID(
  SaveFile_::RefID refID, const uint8_t& type)
{
  for (auto& form : this->changeForms) {
    if ((form.type & 0b00111111) == type &&
        form.formID == refID) /// Upper 2 bits represent the size of the data
                              /// lengths: zero them
      return &form;
  }
  return nullptr;
}

SaveFile_::GlobalVariables::GlobalVariable*
SaveFile_::SaveFile::GetGlobalvariableByRefID(SaveFile_::RefID& refID)
{
  GlobalData& gData = this->globalDataTable1[GLOBAL_VARIABLES_INDEX];

  if (gData.type != GLOBAL_VARIABLES_INDEX)
    return nullptr;

  GlobalVariables* globalsVar =
    reinterpret_cast<GlobalVariables*>(gData.data.get());

  if (!globalsVar)
    return nullptr;

  for (auto& gVar : globalsVar->globals) {
    if (gVar.formID == refID) {
      return &gVar;
    }
  }
  return nullptr;
}

int64_t SaveFile_::SaveFile::FindIndexInFormIdArray(uint32_t refID)
{
  for (uint32_t i = 0; i < this->formIDArray.size(); ++i) {
    if (this->formIDArray[i] == refID) {
      return i;
    }
  }
  return -1;
}

void SaveFile_::SaveFile::OverwritePluginInfo(
  std::vector<std::string>& newPluginNames)
{
  uint32_t oldSize = this->pluginInfoSize;

  this->pluginInfoSize = 1;
  this->pluginInfo.numPlugins = 0;
  this->pluginInfo.pluginsName.clear();

  this->pluginInfo.numPlugins = static_cast<uint8_t>(newPluginNames.size());

  for (auto& plugin : newPluginNames) {
    this->pluginInfo.pluginsName.push_back(plugin);
    this->pluginInfoSize += uint32_t(2 + plugin.size());
  }

  uint32_t addSize = this->pluginInfoSize - oldSize;

  this->fileLocationTable.formIDArrayCountOffset += addSize;
  this->fileLocationTable.unknownTable3Offset += addSize;
  this->fileLocationTable.globalDataTable1Offset += addSize;
  this->fileLocationTable.globalDataTable2Offset += addSize;
  this->fileLocationTable.changeFormsOffset += addSize;
  this->fileLocationTable.globalDataTable3Offset += addSize;
}
