#include "MovementValidation.h"
#include "FormDesc.h"
#include "MpActor.h"
#include "NiPoint3.h"
#include "PartOne.h"
#include "TeleportMessage2.h"
#include <cmath>
#include <nlohmann/json.hpp>
#include <optional>
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

  // thuum docs/verbs/console-commands.md, COC: a move the bounds below refuse
  // may be the one jump the server permitted, which passes; while that jump
  // waits, a move that is not it (a report from the loading screen) is
  // dropped without sending the player back. Nothing when none waits.
  const auto permittedJump = [&]() -> std::optional<bool> {
    if (!isMe) {
      return std::nullopt;
    }
    const uint32_t cellOrWorld = newCellOrWorld.ToFormId(espmFiles);
    switch (
      partOne.CheckJump(actor->GetFormId(), cellOrWorld, newPos.x, newPos.y)) {
      case PartOne::JumpCheck::Landed:
        spdlog::info("MovementValidation: {:x} made its permitted jump to "
                     "{:x} at ({:.0f}, {:.0f}, {:.0f})",
                     actor->GetFormId(), cellOrWorld, newPos.x, newPos.y,
                     newPos.z);
        return true;
      case PartOne::JumpCheck::Waiting:
        spdlog::info("MovementValidation: {:x} reported {:x} at ({:.0f}, "
                     "{:.0f}) while its permitted jump waits; dropped",
                     actor->GetFormId(), cellOrWorld, newPos.x, newPos.y);
        return false;
      default:
        return std::nullopt;
    }
  };

  if (currentCellOrWorld != newCellOrWorld ||
      (currentPos - newPos).SqrLength() >= kSqrMaxDistance) {
    if (const auto jump = permittedJump()) {
      return *jump;
    }
    return snapBack();
  }

  // thuum docs/verbs/movement-speed.md: a player's own actor also spends its
  // ground speed budget
  if (isMe) {
    const float ground =
      std::hypot(newPos.x - currentPos.x, newPos.y - currentPos.y);
    if (!partOne.SpendMovementBudget(actor->GetFormId(), ground)) {
      if (const auto jump = permittedJump()) {
        return *jump;
      }
      spdlog::warn("MovementValidation - E_MOVE_SPEED: {:x} moved {:.0f} "
                   "units over the ground beyond its budget; snapped back",
                   actor->GetFormId(), ground);
      return snapBack();
    }
  }
  return true;
}

} // namespace MovementValidation
