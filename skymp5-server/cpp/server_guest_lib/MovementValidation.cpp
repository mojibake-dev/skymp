#include "MovementValidation.h"
#include "FormDesc.h"
#include "MpActor.h"
#include "NiPoint3.h"
#include "PartOne.h"
#include "TeleportMessage2.h"
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>

namespace MovementValidation {

namespace {
// a player's step beyond its ground speed budget, recorded, counted per actor
// and logged at the first and each power of two after
std::unordered_map<uint32_t, uint64_t> g_overBudget;
}

bool Validate(PartOne& partOne, const NiPoint3& currentPos,
              const NiPoint3& currentRot, const FormDesc& currentCellOrWorld,
              const NiPoint3& newPos, const FormDesc& newCellOrWorld,
              Networking::UserId userId, MpActor* actor,
              const std::vector<std::string>& espmFiles, bool* placed,
              bool hostedCellChange)
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

  // a position that is no place (not finite) breaks the world: dropped,
  // nothing sent back (the wire's own validate refuses it before here on the
  // network path; this holds for the C++ callers)
  if (!std::isfinite(newPos.x) || !std::isfinite(newPos.y) ||
      !std::isfinite(newPos.z)) {
    return false;
  }

  // a move into another cell or worldspace, or a jump no step makes (4096
  // units or more: an exterior cell's side, wire-rules movement::CELL_SIDE)
  const bool jumped = currentCellOrWorld != newCellOrWorld ||
    (currentPos - newPos).SqrLength() >= kSqrMaxDistance;

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
    // for a while after its swap; a report from elsewhere in the same cell is
    // the mod's own script undoing the server's swap, and the player is sent
    // back to where the server has it. This hold is the one place a player's
    // own report is overridden: the swap is the server's own move of two
    // players at once, which the mod's script in one game would otherwise
    // undo for both. A move into another cell or a jump is the player's
    // game's own, recorded below
    if (!jumped &&
        partOne.PinnedAway(actor->GetFormId(), cellOrWorld, newPos)) {
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
        // a report elsewhere while the permit waits is the player's own
        // game's move all the same (a loading screen's, with no cell, is
        // dropped below); dropping every one would freeze the player on the
        // other screens for the permit's 60 s
        return std::nullopt;
      default:
        return std::nullopt;
    }
  };

  // thuum docs/verbs/cell-host.md, the cell-change rule: a hosted actor's
  // move into another cell its host vouches for (a door, a grid edge) is
  // the one case these bounds stand aside for; the caller judged it
  if (!isMe && hostedCellChange) {
    return true;
  }

  if (jumped) {
    if (const auto jump = permittedJump()) {
      return *jump;
    }
    // the player's own game moving it into another cell or worldspace, or by
    // a jump (a jail, a Black Book's Apocrypha, a carriage, fast travel, a
    // script's MoveTo), is never sent back: the server records the move, and
    // the other games place the figure there instead of gliding it. What the
    // server still holds for a player's own report: its own teleport's
    // arrival, while it waits (a report from elsewhere is dropped, nothing
    // sent back), a report at no place or into no cell it knows (dropped,
    // nothing sent back), and a bond's hold within the same cell (above).
    // Another game's actor keeps the hosted rules (the cell-change rule
    // above; dropped otherwise)
    if (isMe) {
      // a report into no cell the server knows (worldOrCell 0, a loading
      // screen's; a form that is no CELL or WRLD; an exterior square with no
      // cell) breaks the world: dropped, nothing sent back
      if (partOne.HasEspm() &&
          !partOne.worldState.CellOf(newCellOrWorld.ToFormId(espmFiles),
                                     newPos)) {
        spdlog::info("[verb:movement] {:x} reported {:x} at ({:.0f}, {:.0f}, "
                     "{:.0f}), no cell the server knows; dropped, not sent "
                     "back",
                     actor->GetFormId(), newCellOrWorld.ToFormId(espmFiles),
                     newPos.x, newPos.y, newPos.z);
        return false;
      }
      spdlog::info("[verb:movement] {:x}'s own game moved it from {:x} at "
                   "({:.0f}, {:.0f}, {:.0f}) to {:x} at ({:.0f}, {:.0f}, "
                   "{:.0f}): recorded, not sent back (ADR-079 decision 7)",
                   actor->GetFormId(), currentCellOrWorld.ToFormId(espmFiles),
                   currentPos.x, currentPos.y, currentPos.z,
                   newCellOrWorld.ToFormId(espmFiles), newPos.x, newPos.y,
                   newPos.z);
      markPlaced();
      return true;
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
      // a step beyond the budget is the player's own game's move too (a
      // script's MoveTo within the cell, a lag burst, Whirlwind Sprint),
      // recorded; the server's disagreement is a logged line, never a
      // send-back. The step is relayed alone, so the other games glide the
      // figure there as movement, where a move into another cell or
      // worldspace and the server's own jumps are placed (ActorJump);
      // markPlaced() here would make it a snap
      const auto n = ++g_overBudget[actor->GetFormId()];
      if ((n & (n - 1)) == 0) {
        spdlog::warn("MovementValidation - E_MOVE_SPEED: {:x} moved {:.0f} "
                     "units over the ground beyond its budget; recorded, not "
                     "sent back (ADR-079 decision 7; {} such step(s))",
                     actor->GetFormId(), ground, n);
      }
      return true;
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
