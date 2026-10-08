#include "ConsoleCommands.h"
#include "ConsoleOutputMessage.h"
#include "MpActor.h"
#include "WorldState.h"
#include "papyrus-vm/Utils.h"
#include "script_classes/PapyrusActor.h"
#include "script_classes/PapyrusObjectReference.h"
#include "script_objects/EspmGameObject.h"
#include "wire_bridge_cxx/rules.h"
#include <cmath>
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

void ExecuteMp(MpActor& caller,
               const std::vector<ConsoleCommands::Argument>& args)
{
  auto subcmd = args.at(1).GetString();
  if (!Utils::stricmp(subcmd.data(), "disable")) {
    return ExecuteDisable(caller, args);
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
// the server makes for an actor)
void ExecuteMoveTo(MpActor& caller,
                   const std::vector<ConsoleCommands::Argument>& args)
{
  MpObjectReference& target = ReferenceOf(caller, args.at(0).GetInteger());
  MpObjectReference& destination =
    caller.GetParent()->GetFormAt<MpObjectReference>(
      static_cast<uint32_t>(args.at(1).GetInteger()));
  PapyrusObjectReference papyrusObjectReference;
  (void)papyrusObjectReference.MoveTo(target.ToVarValue(),
                                      { destination.ToVarValue(),
                                        VarValue(0.0), VarValue(0.0),
                                        VarValue(0.0), VarValue(true) });
}

// every player an owner when the server says so (the lab), else the rank the
// player's record keeps
uint8_t RankOf(const MpActor& me)
{
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
}

// thuum docs/verbs/console-commands.md: the server's table (wire-rules
// console) decides whether the caller's rank may run the command and whether
// the server runs it; the caller's console prints the server's line either
// way (ConsoleOutput). A command that fails prints why.
void ConsoleCommands::Execute(
  MpActor& me, const std::string& consoleCommandName,
  const std::vector<ConsoleCommands::Argument>& args)
{
  const auto decision =
    skymp::rules::console_decide(rust::Str(consoleCommandName), RankOf(me));
  if (decision != skymp::rules::ConsoleDecision::Run) {
    const std::string line(skymp::rules::console_refusal_line(decision));
    spdlog::info("ConsoleCommands: {:x} ran '{}': {}", me.GetFormId(),
                 consoleCommandName, line);
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
      ExecuteMp(me, args);
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
  Reply(me, consoleCommandName + " done", false);
}
