#include "MovementValidation.h"
#include "FormDesc.h"
#include "MpActor.h"
#include "NiPoint3.h"
#include "PartOne.h"
#include "TeleportMessage2.h"
#include <cmath>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace MovementValidation {

bool Validate(PartOne& partOne, const NiPoint3& currentPos,
              const NiPoint3& currentRot, const FormDesc& currentCellOrWorld,
              const NiPoint3& newPos, const FormDesc& newCellOrWorld,
              Networking::UserId userId, MpActor* actor,
              const std::vector<std::string>& espmFiles)
{
  constexpr float kSqrMaxDistance = 4096.f * 4096.f;

  PartOneSendTargetWrapper& sendTarget = partOne.GetSendTarget();

  // Not doing this to any NPCs at this moment, yet we might consider to
  const bool isMe = actor && partOne.serverState.ActorByUser(userId) == actor;

  const auto snapBack = [&] {
    if (isMe) {
      TeleportMessage2 msg;
      msg.pos = { currentPos[0], currentPos[1], currentPos[2] };
      msg.rot = { currentRot[0], currentRot[1], currentRot[2] };
      msg.worldOrCell = currentCellOrWorld.ToFormId(espmFiles);
      sendTarget.Send(userId, msg, true);
    }
    return false;
  };

  if (currentCellOrWorld != newCellOrWorld ||
      (currentPos - newPos).SqrLength() >= kSqrMaxDistance) {
    return snapBack();
  }

  // thuum docs/verbs/movement-speed.md: a player's own actor also spends its
  // ground speed budget
  if (isMe) {
    const float ground =
      std::hypot(newPos.x - currentPos.x, newPos.y - currentPos.y);
    if (!actor->GetMovementBudget().Spend(ground,
                                          MovementBudget::Clock::now())) {
      spdlog::warn("MovementValidation - E_MOVE_SPEED: {:x} moved {:.0f} "
                   "units over the ground beyond its budget; snapped back",
                   actor->GetFormId(), ground);
      return snapBack();
    }
  }
  return true;
}

} // namespace MovementValidation
