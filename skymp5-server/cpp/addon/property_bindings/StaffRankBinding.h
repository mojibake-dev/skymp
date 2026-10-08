#pragma once
#include "PropertyBinding.h"

// thuum docs/verbs/console-commands.md: a player's staff rank, TES3MP's four
// (0 player, 1 moderator, 2 admin, 3 owner). The gamemode names its staff
// through this property; the server's console table reads the rank.
class StaffRankBinding : public PropertyBinding
{
public:
  std::string GetPropertyName() const override { return "staffRank"; }
  Napi::Value Get(Napi::Env env, ScampServer& scampServer,
                  uint32_t formId) override;
  void Set(Napi::Env env, ScampServer& scampServer, uint32_t formId,
           Napi::Value newValue) override;
};
