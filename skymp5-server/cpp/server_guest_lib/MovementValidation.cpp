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
              const std::vector<std::string>& espmFiles, bool* placed)
{
  constexpr float kSqrMaxDistance = 4096.f * 4096.f;
  const auto markPlaced = [&] {
    if (placed) {
      *placed = true;
    }
  };

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

  // thuum docs/verbs/movement-speed.md, arrivals: after a teleport the server
  // made, a report from where it put the player is the arrival and passes;
  // one from anywhere else was sent before the move and is dropped, the
  // player not sent back (the reliable teleport is on its way)
  if (isMe) {
    const uint32_t cellOrWorld = newCellOrWorld.ToFormId(espmFiles);
    switch (partOne.CheckArrival(actor->GetFormId(), cellOrWorld, newPos.x,
                                 newPos.y)) {
      case PartOne::JumpCheck::Landed:
        markPlaced();
        return true;
      case PartOne::JumpCheck::Waiting:
        return false;
      default:
        break;
    }
    // thuum docs/verbs/shadowbond.md: a place the server holds the player at
    // for a while after its swap; a report from elsewhere is a move the
    // server did not author (the mod's own script), and the player is sent
    // back to where the server has it
    if (partOne.PinnedAway(actor->GetFormId(), cellOrWorld, newPos)) {
      spdlog::info("MovementValidation: {:x} reported ({:.0f}, {:.0f}) away "
                   "from the place the server holds it at; sent back",
                   actor->GetFormId(), newPos.x, newPos.y);
      return snapBack();
    }
  }

  // thuum docs/verbs/console-commands.md, COC: a move the bounds below refuse
  // may be the one jump the server permitted, which passes; while that jump
  // waits, a move that is not it (a report from the loading screen) is
  // dropped without sending the player back. Nothing when none waits.
  const auto permittedJump = [&]() -> std::optional<bool> {
    if (!isMe) {
      return std::nullopt;
    }
    const uint32_t cellOrWorld = newCellOrWorld.ToFormId(espmFiles);
    // thuum docs/verbs/cast-teleport.md: the jump a teleport spell's cast
    // permits, within its reach of where the caster stood
    if (partOne.CheckCastJump(actor->GetFormId(), cellOrWorld, newPos) ==
        PartOne::JumpCheck::Landed) {
      spdlog::info("MovementValidation: {:x} made its cast teleport's jump to "
                   "{:x} at ({:.0f}, {:.0f}, {:.0f})",
                   actor->GetFormId(), cellOrWorld, newPos.x, newPos.y,
                   newPos.z);
      // the look on other screens: the figure placed there, the vanish
      // stopped, the appear and the sound played
      partOne.PlayTeleportLanding(*actor, newPos);
      markPlaced();
      return true;
    }
    switch (
      partOne.CheckJump(actor->GetFormId(), cellOrWorld, newPos.x, newPos.y)) {
      case PartOne::JumpCheck::Landed:
        spdlog::info("MovementValidation: {:x} made its permitted jump to "
                     "{:x} at ({:.0f}, {:.0f}, {:.0f})",
                     actor->GetFormId(), cellOrWorld, newPos.x, newPos.y,
                     newPos.z);
        markPlaced();
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
    // thuum docs/verbs/cast-teleport.md, the look on other screens: a
    // teleport's landing the budget takes never reaches the jump's permit
    // above (most of them: b-teleport's 550 units, every landing of
    // playtest fourteen), so a step no run makes between two reports lands
    // a look waiting for it (nothing when none waits)
    if (PartOne::TeleportLookLands(ground)) {
      partOne.PlayTeleportLanding(*actor, newPos);
      // and the teleport snap (docs/verbs/cast-teleport.md; the swap's two
      // landings, docs/verbs/shadowbond.md): the same step, within the cast
      // jump that waits for this actor, is that jump's landing, which the
      // other games place instead of gliding (playtest twenty-three: a jump
      // under 1024 units slid on every other screen). The permit stands: it
      // is for a move the bounds refuse, which this one was not
      const uint32_t cellOrWorld = newCellOrWorld.ToFormId(espmFiles);
      if (partOne.CastJumpLands(actor->GetFormId(), cellOrWorld, newPos)) {
        spdlog::info("MovementValidation: {:x} landed its permitted jump "
                     "within its budget, {:.0f} units to {:x} at ({:.0f}, "
                     "{:.0f}, {:.0f}); placed for the other games",
                     actor->GetFormId(), ground, cellOrWorld, newPos.x,
                     newPos.y, newPos.z);
        markPlaced();
      }
    }
  }
  return true;
}

} // namespace MovementValidation
