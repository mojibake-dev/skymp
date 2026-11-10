#pragma once
#include "Script.h"

#pragma pack(push, 1)

namespace espm {

struct ScriptData
{
  int16_t version = 0;   // [2..5]
  int16_t objFormat = 0; // [1..2]
  std::vector<Script> scripts;

  // thuum ADR-068 rung 5: a QUST's alias scripts, its VMAD's Alias Scripts
  // Section after the fragments (UESP, "Skyrim Mod:Mod File Format/VMAD
  // Field"): each alias that carries scripts, by its alias id, with its
  // scripts and their properties as `scripts` holds the quest's own. Empty
  // for any other record, and for a QUST whose VMAD ends after its scripts;
  // a section that stops reading short keeps the aliases read whole before
  // it
  struct AliasScripts
  {
    int16_t aliasId = -1;
    std::vector<Script> scripts;
  };
  std::vector<AliasScripts> aliases;
};

}

#pragma pack(pop)
