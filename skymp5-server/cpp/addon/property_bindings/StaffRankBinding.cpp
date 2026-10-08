#include "StaffRankBinding.h"
#include "NapiHelper.h"

Napi::Value StaffRankBinding::Get(Napi::Env env, ScampServer& scampServer,
                                  uint32_t formId)
{
  auto& partOne = scampServer.GetPartOne();
  auto& actor = partOne->worldState.GetFormAt<MpActor>(formId);
  return Napi::Number::New(env, actor.GetStaffRank());
}

void StaffRankBinding::Set(Napi::Env env, ScampServer& scampServer,
                           uint32_t formId, Napi::Value newValue)
{
  const uint32_t rank = NapiHelper::ExtractUInt32(newValue, "newStaffRank");
  if (rank > 3) {
    throw std::runtime_error(
      "staffRank is 0 player, 1 moderator, 2 admin or 3 owner");
  }
  auto& partOne = scampServer.GetPartOne();
  auto& actor = partOne->worldState.GetFormAt<MpActor>(formId);
  actor.SetStaffRank(static_cast<uint8_t>(rank));
}
