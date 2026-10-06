#pragma once
#include <cstdint>

namespace RE {
class Actor;
}

// RaceMenu's interfaces for other SKSE plugins, declared here for their ABI
// only (thuum docs/verbs/racemenu-sync.md). The source of the layout is
// RaceMenu 0.4.20.0's own header for plugin authors,
// ModderResource/IPluginInterface.h: IPluginInterface (lines 24-32),
// IInterfaceMap (34-40), InterfaceExchangeMessage (42-50), IPresetInterface
// (467-492). The method order is the vtable order; nothing else is used.
namespace RaceMenu {

class IPluginInterface
{
public:
  IPluginInterface() {}
  virtual ~IPluginInterface() {}

  virtual uint32_t GetVersion() = 0;
  virtual void Revert() = 0;
};

class IInterfaceMap
{
public:
  virtual IPluginInterface* QueryInterface(const char* name) = 0;
  virtual bool AddInterface(const char* name,
                            IPluginInterface* pluginInterface) = 0;
  virtual IPluginInterface* RemoveInterface(const char* name) = 0;
};

// Sent to skee through SKSE messaging; skee writes its interface map into it
// before Dispatch returns (RaceMenu's public source, skee64/main.cpp
// InterfaceExchangeMessageHandler; HYPOTHESIS for 0.4.20.0 until the lab
// reads a Preset version through it)
struct InterfaceExchangeMessage
{
  enum
  {
    kMessage_ExchangeInterface = 0x9E3779B9
  };

  IInterfaceMap* interfaceMap = nullptr;
};

class IPresetInterface : public IPluginInterface
{
public:
  enum ApplyTypes
  {
    kPresetApplyFace = 0,
    kPresetApplyOverrides = 1 << 0,
    kPresetApplyBodyMorphs = 1 << 1,
    kPresetApplyTransforms = 1 << 2,
    kPresetApplySkinOverrides = 1 << 3,
    kPresetApplyAll = kPresetApplyFace | kPresetApplyOverrides |
      kPresetApplyBodyMorphs | kPresetApplyTransforms |
      kPresetApplySkinOverrides
  };

  virtual bool SavePreset(const char* filePath, const char* tintPath,
                          RE::Actor* actor) = 0;
  virtual bool LoadPreset(const char* filePath, const char* tintPath,
                          RE::Actor* actor,
                          ApplyTypes applyTypes = kPresetApplyAll) = 0;
};

}
