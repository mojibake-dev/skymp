#include "ConsoleCommands.h"
#include "ConsoleOutputMessage.h"
#include "MpActor.h"
#include "PartOne.h"
#include "SpSnippet.h"
#include "WorldState.h"
#include "libespm/CELL.h"
#include "libespm/GroupUtils.h"
#include "libespm/Utils.h"
#include "papyrus-vm/CIString.h"
#include "papyrus-vm/Utils.h"
#include "script_classes/PapyrusActor.h"
#include "script_classes/PapyrusObjectReference.h"
#include "script_objects/EspmGameObject.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <string>

ConsoleCommands::Argument::Argument()
{
  data = 0;
}

ConsoleCommands::Argument::Argument(int64_t integer)
{
  data = integer;
}

ConsoleCommands::Argument::Argument(const std::string& str)
{
  data = str;
}

ConsoleCommands::Argument::Argument(
  const std::variant<int64_t, std::string>& data_)
{
  data = data_;
}

bool ConsoleCommands::Argument::IsInteger() const noexcept
{
  return data.index() == 0;
}

bool ConsoleCommands::Argument::IsString() const noexcept
{
  return data.index() == 1;
};

int64_t ConsoleCommands::Argument::GetInteger() const
{
  if (!IsInteger())
    throw std::runtime_error(
      "ConsoleCommands::Argument - Expected to be Integer");
  return std::get<int64_t>(data);
}

const std::string& ConsoleCommands::Argument::GetString() const
{
  if (!IsString())
    throw std::runtime_error(
      "ConsoleCommands::Argument - Expected to be String");
  return std::get<std::string>(data);
}

namespace {

void ExecuteAddItem(MpActor& caller,
                    const std::vector<ConsoleCommands::Argument>& args)
{
  const auto targetId = static_cast<uint32_t>(args.at(0).GetInteger());
  const auto itemId = static_cast<uint32_t>(args.at(1).GetInteger());
  const auto count = static_cast<int32_t>(args.at(2).GetInteger());

  MpObjectReference& target = (targetId == 0x14)
    ? caller
    : caller.GetParent()->GetFormAt<MpObjectReference>(targetId);

  auto& br = caller.GetParent()->GetEspm().GetBrowser();

  PapyrusObjectReference papyrusObjectReference;
  auto aItem =
    VarValue(std::make_shared<EspmGameObject>(br.LookupById(itemId)));
  auto aCount = VarValue(count);
  auto aSilent = VarValue(false);
  (void)papyrusObjectReference.AddItem(target.ToVarValue(),
                                       { aItem, aCount, aSilent });
}

void ExecuteEquipItem(MpActor& caller,
                      const std::vector<ConsoleCommands::Argument>& args)
{
  const auto targetId = static_cast<uint32_t>(args.at(0).GetInteger());
  const auto itemId = static_cast<uint32_t>(args.at(1).GetInteger());

  MpObjectReference& target = (targetId == 0x14)
    ? caller
    : caller.GetParent()->GetFormAt<MpObjectReference>(targetId);

  auto& br = caller.GetParent()->GetEspm().GetBrowser();

  PapyrusActor papyrusActor;
  auto aItem =
    VarValue(std::make_shared<EspmGameObject>(br.LookupById(itemId)));

  auto aForce = VarValue(false);
  auto aSilent = VarValue(false);
  (void)papyrusActor.EquipItem(target.ToVarValue(),
                               { aItem, aForce, aSilent });
}

void ExecutePlaceAtMe(MpActor& caller,
                      const std::vector<ConsoleCommands::Argument>& args)
{
  const auto targetId = static_cast<uint32_t>(args.at(0).GetInteger());
  const auto baseFormId = static_cast<uint32_t>(args.at(1).GetInteger());

  MpObjectReference& target = (targetId == 0x14)
    ? caller
    : caller.GetParent()->GetFormAt<MpObjectReference>(targetId);

  auto& br = caller.GetParent()->GetEspm().GetBrowser();

  PapyrusObjectReference papyrusObjectReference;
  auto aBaseForm =
    VarValue(std::make_shared<EspmGameObject>(br.LookupById(baseFormId)));
  auto aCount = VarValue(1);
  auto aForcePersist = VarValue(false);
  auto aInitiallyDisabled = VarValue(false);
  (void)papyrusObjectReference.PlaceAtMe(
    target.ToVarValue(),
    { aBaseForm, aCount, aForcePersist, aInitiallyDisabled });
}

void ExecuteDisable(MpActor& caller,
                    const std::vector<ConsoleCommands::Argument>& args)
{
  const auto targetId = static_cast<uint32_t>(args.at(0).GetInteger());

  MpObjectReference& target = (targetId == 0x14)
    ? caller
    : caller.GetParent()->GetFormAt<MpObjectReference>(targetId);

  // TODO: allow disable for all
  if (target.GetFormId() >= 0xff000000 ||
      dynamic_cast<MpActor*>(&target) != nullptr) {
    target.Disable();
  }
}

// A typed number: an integer as the wire carries one, or the text of one the
// console typed with a fraction. Anything else, or a value that is not finite,
// fails the command with one line.
double NumberOf(const ConsoleCommands::Argument& argument)
{
  double value = 0;
  if (argument.IsInteger()) {
    value = static_cast<double>(argument.GetInteger());
  } else {
    const std::string& text = argument.GetString();
    size_t used = 0;
    try {
      value = std::stod(text, &used);
    } catch (std::exception&) {
      used = 0;
    }
    if (used == 0 || used != text.size()) {
      throw std::runtime_error("the value is not a number");
    }
  }
  if (!std::isfinite(value)) {
    throw std::runtime_error("the value is not a number");
  }
  return value;
}

// thuum docs/verbs/console-commands.md: SetAV, ModAV and ForceAV through the
// server's own actor value natives (docs/verbs/actor-values.md: R0 on a
// player). Arguments as Skyrim Platform passes a replaced command's: the
// selected reference (0 or the player for the caller), the actor value's
// name, the value (a number, or its text when the console typed a fraction).
void ExecuteActorValue(MpActor& caller, const char* how,
                       const std::vector<ConsoleCommands::Argument>& args)
{
  const auto targetId = static_cast<uint32_t>(args.at(0).GetInteger());
  MpActor& target = (targetId == 0x14 || targetId == 0)
    ? caller
    : caller.GetParent()->GetFormAt<MpActor>(targetId);
  const std::string& name = args.at(1).GetString();
  const double value = NumberOf(args.at(2));
  PapyrusActor papyrusActor;
  const std::vector<VarValue> arguments{ VarValue(name), VarValue(value) };
  if (!Utils::stricmp(how, "set")) {
    (void)papyrusActor.SetActorValue(target.ToVarValue(), arguments);
  } else if (!Utils::stricmp(how, "mod")) {
    (void)papyrusActor.ModActorValue(target.ToVarValue(), arguments);
  } else {
    (void)papyrusActor.ForceActorValue(target.ToVarValue(), arguments);
  }
}

// The reference a command names: the caller for the player (0x14) or no
// selection (0), else the server's reference of that id
MpObjectReference& ReferenceOf(MpActor& caller, int64_t id)
{
  const auto targetId = static_cast<uint32_t>(id);
  if (targetId == 0x14 || targetId == 0) {
    return caller;
  }
  return caller.GetParent()->GetFormAt<MpObjectReference>(targetId);
}

MpActor& ActorOf(MpActor& caller, int64_t id)
{
  const auto targetId = static_cast<uint32_t>(id);
  if (targetId == 0x14 || targetId == 0) {
    return caller;
  }
  return caller.GetParent()->GetFormAt<MpActor>(targetId);
}

void ExecuteRemoveItem(MpActor& caller,
                       const std::vector<ConsoleCommands::Argument>& args)
{
  MpObjectReference& target = ReferenceOf(caller, args.at(0).GetInteger());
  const auto itemId = static_cast<uint32_t>(args.at(1).GetInteger());
  const auto count = static_cast<int32_t>(args.at(2).GetInteger());
  auto& br = caller.GetParent()->GetEspm().GetBrowser();
  PapyrusObjectReference papyrusObjectReference;
  auto aItem =
    VarValue(std::make_shared<EspmGameObject>(br.LookupById(itemId)));
  (void)papyrusObjectReference.RemoveItem(
    target.ToVarValue(),
    { aItem, VarValue(count), VarValue(false), VarValue::None() });
}

// Enable, as Disable: references the server created and actors
void ExecuteEnable(MpActor& caller,
                   const std::vector<ConsoleCommands::Argument>& args)
{
  MpObjectReference& target = ReferenceOf(caller, args.at(0).GetInteger());
  if (target.GetFormId() < 0xff000000 &&
      dynamic_cast<MpActor*>(&target) == nullptr) {
    throw std::runtime_error("only references the server made, and actors");
  }
  target.Enable();
}

// Kill: the server's death (docs/verbs/hostility-sync.md and m0-death), the
// killer the second reference when the console names one
void ExecuteKill(MpActor& caller,
                 const std::vector<ConsoleCommands::Argument>& args)
{
  MpActor& target = ActorOf(caller, args.at(0).GetInteger());
  MpActor* killer =
    args.size() > 1 ? &ActorOf(caller, args.at(1).GetInteger()) : nullptr;
  if (target.IsDead()) {
    throw std::runtime_error("already dead");
  }
  target.Kill(killer);
}

// Resurrect: up again where it fell (the server's respawn without the
// teleport to the spawn point)
void ExecuteResurrect(MpActor& caller,
                      const std::vector<ConsoleCommands::Argument>& args)
{
  MpActor& target = ActorOf(caller, args.at(0).GetInteger());
  if (!target.IsDead()) {
    throw std::runtime_error("not dead");
  }
  target.Respawn(false);
}

// The axis a console command names: X, Y or Z, case aside
int AxisOf(const ConsoleCommands::Argument& argument)
{
  const std::string& axis = argument.GetString();
  if (axis.size() == 1) {
    switch (axis[0]) {
      case 'x':
      case 'X':
        return 0;
      case 'y':
      case 'Y':
        return 1;
      case 'z':
      case 'Z':
        return 2;
    }
  }
  throw std::runtime_error("the axis is X, Y or Z");
}

// SetPos and SetAngle: one axis of the reference's position or rotation
// (degrees, as the console types them). An actor moves and turns through the
// server's teleport, which its game applies (skymp5-client remoteServer.ts
// turns a teleport's degrees into the engine's radians); any other reference
// through the Papyrus natives SetPosition and SetAngle, which take all three.
void ExecuteSetPos(MpActor& caller,
                   const std::vector<ConsoleCommands::Argument>& args,
                   bool angle)
{
  MpObjectReference& target = ReferenceOf(caller, args.at(0).GetInteger());
  const int axis = AxisOf(args.at(1));
  const double value = NumberOf(args.at(2));
  NiPoint3 pos = target.GetPos();
  NiPoint3 rot = target.GetAngle();
  NiPoint3& v = angle ? rot : pos;
  (axis == 0 ? v.x : axis == 1 ? v.y : v.z) = static_cast<float>(value);
  if (auto* actor = dynamic_cast<MpActor*>(&target)) {
    actor->Teleport(LocationalData{ pos, rot, actor->GetCellOrWorld() });
    return;
  }
  PapyrusObjectReference papyrusObjectReference;
  const std::vector<VarValue> xyz{ VarValue(static_cast<double>(v.x)),
                                   VarValue(static_cast<double>(v.y)),
                                   VarValue(static_cast<double>(v.z)) };
  if (angle) {
    (void)papyrusObjectReference.SetAngle(target.ToVarValue(), xyz);
  } else {
    (void)papyrusObjectReference.SetPosition(target.ToVarValue(), xyz);
  }
}

// MoveTo: to the named reference, through the Papyrus native (a teleport
// the server makes for an actor). `moveto player` names the caller: Skyrim
// Platform reads the word "player" as a hex id and passes 0
// (x-moveto-probe 20261008-225615: "Form with id 0x0 doesn't exist")
void ExecuteMoveTo(MpActor& caller,
                   const std::vector<ConsoleCommands::Argument>& args)
{
  MpObjectReference& target = ReferenceOf(caller, args.at(0).GetInteger());
  MpObjectReference& destination =
    ReferenceOf(caller, args.at(1).GetInteger());
  PapyrusObjectReference papyrusObjectReference;
  (void)papyrusObjectReference.MoveTo(target.ToVarValue(),
                                      { destination.ToVarValue(),
                                        VarValue(0.0), VarValue(0.0),
                                        VarValue(0.0), VarValue(true) });
}

// thuum docs/verbs/console-commands.md, COC: a cell as the server's load
// order has it: its editor id, which the game's COC takes, and where a jump
// there lands (an interior cell, or an exterior cell's worldspace and grid
// square)
struct NamedCell
{
  std::string editorId;
  bool interior = true;
  uint32_t cellOrWorld = 0;
  int16_t gridX = 0;
  int16_t gridY = 0;
};

std::optional<NamedCell> CellOf(const espm::CombineBrowser& br,
                                espm::CompressedFieldsCache& cache,
                                const espm::LookupResult& found)
{
  if (!found.rec || !espm::utils::Is<espm::CELL>(found.rec->GetType())) {
    return std::nullopt;
  }
  NamedCell cell;
  const char* editorId = found.rec->GetEditorId(cache);
  cell.editorId = editorId ? editorId : "";
  if (cell.editorId.empty()) {
    return std::nullopt;
  }
  // an interior cell sits in the CELL group, an exterior one under its
  // worldspace (UESP, "Skyrim Mod:Mod File Format", Groups)
  const auto worldGroup = espm::GetExteriorWorldGroup(br, found.rec);
  if (!worldGroup) {
    cell.cellOrWorld = found.ToGlobalId(found.rec->GetId());
    return cell;
  }
  uint32_t rawWorld = 0;
  int32_t x = 0, y = 0;
  if (!worldGroup->GetParentWRLD(rawWorld) ||
      !reinterpret_cast<const espm::CELL*>(found.rec)->GetGrid(x, y, cache) ||
      x < std::numeric_limits<int16_t>::min() ||
      x > std::numeric_limits<int16_t>::max() ||
      y < std::numeric_limits<int16_t>::min() ||
      y > std::numeric_limits<int16_t>::max()) {
    return std::nullopt;
  }
  cell.interior = false;
  cell.cellOrWorld = found.ToGlobalId(rawWorld);
  cell.gridX = static_cast<int16_t>(x);
  cell.gridY = static_cast<int16_t>(y);
  return cell;
}

// The cell with an editor id, case aside, as the game's console finds one;
// the last file in the load order that has a cell of that name wins
std::optional<NamedCell> CellNamed(WorldState& worldState,
                                   const std::string& name)
{
  const auto& br = worldState.GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  const CIString wanted(name.begin(), name.end());
  for (const auto& found : br.GetDistinctRecordsByType("CELL")) {
    const char* editorId = found.rec->GetEditorId(cache);
    if (editorId && CIString(editorId) == wanted) {
      return CellOf(br, cache, found);
    }
  }
  return std::nullopt;
}

// CenterOnCell (COC): the caller's own game goes to the cell as its console
// would (the engine picks the spot), and the server permits that one jump
// (its movement bounds refuse any other cell change) and records where the
// game lands. The cell comes as Skyrim Platform passes a replaced command's
// parameter: the typed name, or the form id the game found for it (its
// ConsoleApi.cpp GetTypedArg); either way the server's load order must know
// the cell. The selected reference is ignored, as the game's COC ignores it.
void ExecuteCenterOnCell(PartOne& partOne, MpActor& caller,
                         const std::vector<ConsoleCommands::Argument>& args)
{
  if (args.size() < 2) {
    throw std::runtime_error("COC needs a cell's name");
  }
  const auto& named = args[1];
  WorldState& worldState = *caller.GetParent();
  const auto cell = named.IsInteger()
    ? CellOf(worldState.GetEspm().GetBrowser(), worldState.GetEspmCache(),
             worldState.GetEspm().GetBrowser().LookupById(
               static_cast<uint32_t>(named.GetInteger())))
    : CellNamed(worldState, named.GetString());
  if (!cell) {
    throw std::runtime_error(
      named.IsInteger() ? fmt::format("no cell {:x}", named.GetInteger())
                        : "no cell named " + named.GetString());
  }
  if (cell->interior) {
    partOne.PermitJump(caller.GetFormId(), cell->cellOrWorld);
  } else {
    partOne.PermitJump(caller.GetFormId(), cell->cellOrWorld, cell->gridX,
                       cell->gridY);
  }
  // which of the two the console sent is a lab observation (the verb doc)
  spdlog::info("ConsoleCommands: {:x} goes to {} ({} {:x}{}), named by {}; "
               "its jump there is permitted",
               caller.GetFormId(), cell->editorId,
               cell->interior ? "interior" : "worldspace", cell->cellOrWorld,
               cell->interior
                 ? std::string()
                 : fmt::format(" square ({}, {})", cell->gridX, cell->gridY),
               named.IsInteger() ? "form id" : "text");
  const std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    snippetArgs{ cell->editorId };
  SpSnippet("Debug", "CenterOnCell", snippetArgs)
    .Execute(&caller, SpSnippetMode::kNoReturnResult);
}

// ToggleCollision (TCL, an admin's; thuum docs/verbs/console-commands.md,
// M1.1, Eli 2026-10-09): the caller's own game toggles its player's
// collision, as its console would (Debug.ToggleCollisions, sent as COC's
// Debug.CenterOnCell is). The state is the game's alone: the server models
// no collision, and its movement rule bounds every move's speed as before.
void ExecuteToggleCollision(MpActor& caller)
{
  spdlog::info("ConsoleCommands: {:x} toggles its game's collision",
               caller.GetFormId());
  const std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    none;
  SpSnippet("Debug", "ToggleCollisions", none)
    .Execute(&caller, SpSnippetMode::kNoReturnResult);
}

// The rank the gamemode recorded for the player wins; without one, every
// player is an owner when the server says so (enableConsoleCommandsForAll,
// the lab's setting), else the record's fallback (consoleCommandsAllowed
// reads as admin, anything else as player)
uint8_t RankOf(const MpActor& me)
{
  if (auto recorded = me.GetRecordedStaffRank()) {
    return *recorded;
  }
  if (auto worldState = me.GetParent()) {
    if (worldState->enableConsoleCommandsForAll) {
      return 3;
    }
  }
  return me.GetStaffRank();
}

void Reply(MpActor& caller, std::string text, bool refused)
{
  ConsoleOutputMessage out;
  out.text = std::move(text);
  out.refused = refused;
  caller.SendToUser(out, true);
}

// The player online with a number (its profile id; `mp list` shows them)
MpActor& PlayerOnline(PartOne& partOne, MpActor& caller,
                      const ConsoleCommands::Argument& argument)
{
  const double number = NumberOf(argument);
  if (number < 0 || number > std::numeric_limits<int32_t>::max() ||
      number != std::floor(number)) {
    throw std::runtime_error("a player's number is a whole number");
  }
  const auto profileId = static_cast<int32_t>(number);
  for (uint32_t actorId :
       caller.GetParent()->GetActorsByProfileId(profileId)) {
    const auto& form = caller.GetParent()->LookupFormById(actorId);
    auto* actor = form ? form->AsActor() : nullptr;
    if (actor &&
        partOne.serverState.UserByActor(actor) != Networking::InvalidUserId) {
      return *actor;
    }
  }
  throw std::runtime_error(fmt::format("no player {} online", profileId));
}

std::string NameOf(const MpActor& actor)
{
  const auto appearance = actor.GetAppearance();
  return appearance && !appearance->name.empty() ? appearance->name
                                                 : std::string("(no name)");
}

LocationalData WhereIs(const MpActor& actor)
{
  return LocationalData{ actor.GetPos(), actor.GetAngle(),
                         actor.GetCellOrWorld() };
}

// SkyMP's own `mp`, with TES3MP's player commands (thuum
// docs/verbs/console-commands.md; TES3MP CoreScripts commandHandler.lua
// /teleport and /teleportto, logicHandler.lua TeleportToPlayer): `mp list`
// names every player online by its number, `mp tp <n>` brings that player
// to the caller and `mp tpto <n>` takes the caller to it. The server makes
// each teleport, wherever the two are: no figure in the caller's game is
// needed, as the console's own MoveTo needs one.
void ExecuteMp(PartOne& partOne, MpActor& caller,
               const std::vector<ConsoleCommands::Argument>& args)
{
  const std::string& sub = args.at(1).GetString();
  if (!Utils::stricmp(sub.data(), "disable")) {
    return ExecuteDisable(caller, args);
  }
  if (!Utils::stricmp(sub.data(), "list")) {
    for (size_t i = 0, n = partOne.serverState.maxConnectedId; i <= n; ++i) {
      MpActor* actor =
        partOne.serverState.ActorByUser(static_cast<Networking::UserId>(i));
      if (!actor) {
        continue;
      }
      Reply(caller,
            fmt::format("{} {}{} ({:x}) in {}", actor->GetProfileId(),
                        NameOf(*actor), actor == &caller ? " (you)" : "",
                        actor->GetFormId(),
                        actor->GetCellOrWorld().ToString()),
            false);
    }
    return;
  }
  if (!Utils::stricmp(sub.data(), "tp") ||
      !Utils::stricmp(sub.data(), "tpto")) {
    MpActor& other = PlayerOnline(partOne, caller, args.at(2));
    if (&other == &caller) {
      throw std::runtime_error("that number is yours");
    }
    const bool bring = !Utils::stricmp(sub.data(), "tp");
    MpActor& moved = bring ? other : caller;
    MpActor& anchor = bring ? caller : other;
    moved.Teleport(WhereIs(anchor));
    if (bring) {
      Reply(other, fmt::format("{} brought you to them", NameOf(caller)),
            false);
    }
    return;
  }
  throw std::runtime_error("mp knows list, tp <n>, tpto <n> and disable");
}
}

// thuum docs/verbs/console-commands.md: the server's table (wire-rules
// console) decides whether the caller's rank may run the command and whether
// the server runs it; the caller's console prints the server's line either
// way (ConsoleOutput). A command that fails prints why.
void ConsoleCommands::Execute(
  PartOne& partOne, MpActor& me, const std::string& consoleCommandName,
  const std::vector<ConsoleCommands::Argument>& args)
{
  // `mp <sub>`: TES3MP's player commands have their own rows (and ranks)
  // in the table; any other `mp` is judged as `mp`
  std::string judged = consoleCommandName;
  if (!Utils::stricmp(consoleCommandName.data(), "mp") && args.size() > 1 &&
      args[1].IsString()) {
    std::string sub = "mp " + args[1].GetString();
    std::transform(sub.begin(), sub.end(), sub.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (skymp::rules::console_decide(rust::Str(sub), RankOf(me)) !=
        skymp::rules::ConsoleDecision::Unknown) {
      judged = sub;
    }
  }
  const auto decision =
    skymp::rules::console_decide(rust::Str(judged), RankOf(me));
  if (decision != skymp::rules::ConsoleDecision::Run) {
    const std::string line(skymp::rules::console_refusal_line(decision));
    spdlog::info("ConsoleCommands: {:x} ran '{}': {}", me.GetFormId(), judged,
                 line);
    return Reply(me, line, true);
  }
  try {
    const char* name = consoleCommandName.data();
    if (!Utils::stricmp(name, "AddItem")) {
      ExecuteAddItem(me, args);
    } else if (!Utils::stricmp(name, "EquipItem")) {
      ExecuteEquipItem(me, args);
    } else if (!Utils::stricmp(name, "PlaceAtMe")) {
      ExecutePlaceAtMe(me, args);
    } else if (!Utils::stricmp(name, "Disable")) {
      ExecuteDisable(me, args);
    } else if (!Utils::stricmp(name, "Mp")) {
      ExecuteMp(partOne, me, args);
    } else if (!Utils::stricmp(name, "RemoveItem")) {
      ExecuteRemoveItem(me, args);
    } else if (!Utils::stricmp(name, "Enable")) {
      ExecuteEnable(me, args);
    } else if (!Utils::stricmp(name, "Kill")) {
      ExecuteKill(me, args);
    } else if (!Utils::stricmp(name, "Resurrect")) {
      ExecuteResurrect(me, args);
    } else if (!Utils::stricmp(name, "SetPos")) {
      ExecuteSetPos(me, args, false);
    } else if (!Utils::stricmp(name, "SetAngle")) {
      ExecuteSetPos(me, args, true);
    } else if (!Utils::stricmp(name, "MoveTo")) {
      ExecuteMoveTo(me, args);
    } else if (!Utils::stricmp(name, "COC") ||
               !Utils::stricmp(name, "CenterOnCell")) {
      ExecuteCenterOnCell(partOne, me, args);
    } else if (!Utils::stricmp(name, "TCL") ||
               !Utils::stricmp(name, "ToggleCollision")) {
      ExecuteToggleCollision(me);
    } else if (!Utils::stricmp(name, "SetAV") ||
               !Utils::stricmp(name, "SetActorValue")) {
      ExecuteActorValue(me, "set", args);
    } else if (!Utils::stricmp(name, "ModAV") ||
               !Utils::stricmp(name, "ModActorValue")) {
      ExecuteActorValue(me, "mod", args);
    } else if (!Utils::stricmp(name, "ForceAV") ||
               !Utils::stricmp(name, "ForceActorValue")) {
      ExecuteActorValue(me, "force", args);
    } else {
      return Reply(me, "The server does not run this command yet", true);
    }
  } catch (std::exception& e) {
    spdlog::info("ConsoleCommands: {:x} ran '{}' and it failed: {}",
                 me.GetFormId(), consoleCommandName, e.what());
    return Reply(me, std::string("Failed: ") + e.what(), true);
  }
  Reply(me, judged + " done", false);
}
