#include "ActionListener.h"
#include "ConsoleCommandMessage.h"
#include "LockPromptMessage.h"
#include "MsgType.h"
#include "OpenContainerMessage.h"
#include "TestUtils.hpp"
#include "UpdatePropertyMessage.h"
#include "libespm/Loader.h"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum docs/verbs/locks.md (M4): the server's lock state on a reference. The
// game files' locks as lab/esm.py read Skyrim.esm (2026-10-31): Bleak Falls
// Temple 0x371de's bandit chest TreasBanditChest 0x371e9 at (-3420, 732, 575)
// carries an XLOC with level 0 (a Novice lock) and no key, the draugr chest
// 0x3fa69 one with level 25 (Apprentice); Whiterun's barrel 0x20570 has none.
// The Lockpick is MISC 0xa.

namespace {
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kBanditChest = 0x371e9;
constexpr uint32_t kDraugrChest = 0x3fa69;
constexpr uint32_t kBarrel = 0x20570;
constexpr uint32_t kLockpick = 0xa;
constexpr uint32_t kPicker = 0xff000fe1;
constexpr uint32_t kWatcher = 0xff000fe2;
constexpr uint32_t kAdmin = 0xff000fe3;
// RiverwoodAlvorHouseKey (lab/esm.py, Skyrim.esm 2026-10-31): a vanilla key
// the console hands a player for a key lock
constexpr uint32_t kRiverwoodKey = 0x36a3d;

// the picker (user 0) at the chest, the watcher (user 1) 100 units off, in
// the temple
void Enter(PartOne& p)
{
  p.worldState.locksEnabled = true;
  DoConnect(p, 0);
  p.CreateActor(kPicker, { -3420.f, 732.f, 575.f }, 0, kTemple);
  p.SetUserActor(0, kPicker);
  DoConnect(p, 1);
  p.CreateActor(kWatcher, { -3320.f, 732.f, 575.f }, 0, kTemple);
  p.SetUserActor(1, kWatcher);
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kPicker);
  p.DestroyActor(kWatcher);
  p.worldState.locksEnabled = false;
}

void Report(PartOne& p, uint32_t refr, int outcome, int broken)
{
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::LockReport) },
                            { "refr", refr },
                            { "outcome", outcome },
                            { "broken", broken } });
}

template <class T>
std::vector<T> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<T> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<T*>(m.message.get());
    if (m.userId == user && typed) {
      out.push_back(*typed);
    }
  }
  return out;
}

std::vector<UpdatePropertyMessage> LockProps(PartOne& p,
                                             Networking::UserId user)
{
  std::vector<UpdatePropertyMessage> out;
  for (auto& m : SentTo<UpdatePropertyMessage>(p, user)) {
    if (m.propName == "lock") {
      out.push_back(m);
    }
  }
  return out;
}
}

TEST_CASE("The game files' locks read: a Novice chest, an Apprentice chest, "
          "a barrel without one",
          "[Locks][espm]")
{
  PartOne& p = GetPartOne();
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBanditChest);
  // the switch off: no lock anywhere
  p.worldState.locksEnabled = false;
  REQUIRE(!chest.GetLock());
  p.worldState.locksEnabled = true;
  const auto lock = chest.GetLock();
  REQUIRE(lock.has_value());
  REQUIRE(lock->level == 0);
  REQUIRE(lock->key == 0);
  REQUIRE(lock->locked);
  REQUIRE(lock->broken == 0);
  const auto draugr =
    p.worldState.GetFormAt<MpObjectReference>(kDraugrChest).GetLock();
  REQUIRE(draugr.has_value());
  REQUIRE(draugr->level == 25);
  REQUIRE(draugr->locked);
  REQUIRE(!p.worldState.GetFormAt<MpObjectReference>(kBarrel).GetLock());
  p.worldState.locksEnabled = false;
}

TEST_CASE("A locked chest's activation prompts the player's game instead of "
          "opening; a pick report unlocks it for everyone and the next "
          "activation opens; a report on the open chest is refused",
          "[Locks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBanditChest);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  // the counts below are exact: whatever the picker was created holding
  // (pipeline 4155 read ten Lockpicks on it) leaves first
  picker.RemoveAllItems();
  picker.AddItem(kLockpick, 2);

  // the activation meets the lock: a prompt to the picker, no container
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::Activate) },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", kBanditChest },
                                { "isSecondActivation", false } } } });
  REQUIRE(SentTo<LockPromptMessage>(p, 0).size() == 1);
  REQUIRE(SentTo<LockPromptMessage>(p, 0)[0].refr == kBanditChest);
  REQUIRE(SentTo<OpenContainerMessage>(p, 0).empty());
  REQUIRE(!chest.IsOpen());
  REQUIRE(chest.GetLock()->locked);

  // the pick: unlocked on the record, the property to both games
  p.Messages().clear();
  Report(p, kBanditChest, 0, 0);
  REQUIRE(!chest.GetLock()->locked);
  REQUIRE(LockProps(p, 0).size() == 1);
  REQUIRE(LockProps(p, 1).size() == 1);
  REQUIRE(LockProps(p, 1)[0].dataDump.find("\"locked\":false") !=
          std::string::npos);
  REQUIRE(picker.GetInventory().GetItemCount(kLockpick) == 2);

  // the next activation opens the chest
  p.Messages().clear();
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::Activate) },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", kBanditChest },
                                { "isSecondActivation", false } } } });
  REQUIRE(SentTo<LockPromptMessage>(p, 0).empty());
  REQUIRE(SentTo<OpenContainerMessage>(p, 0).size() == 1);
  REQUIRE(chest.IsOpen());
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::Activate) },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", kBanditChest },
                                { "isSecondActivation", true } } } });

  // a report on the open chest changes nothing
  p.Messages().clear();
  Report(p, kBanditChest, 0, 0);
  REQUIRE(LockProps(p, 1).empty());
  REQUIRE(!chest.GetLock()->locked);

  // the record keeps the unlock across a load
  MpChangeForm saved = chest.GetChangeForm();
  REQUIRE(saved.lock.has_value());
  REQUIRE(!saved.lock->locked);
  p.worldState.LoadChangeForm(saved, p.CreateFormCallbacks());
  REQUIRE(!chest.GetLock()->locked);

  // left as found: locked again by the console's Lock, as an admin types it
  MpChangeFormREFR::Lock relocked = *chest.GetLock();
  relocked.locked = true;
  chest.SetLock(relocked);
  REQUIRE(chest.GetLock()->locked);
  Leave(p);
}

TEST_CASE("Broken picks cost Lockpicks and no more than held; a report "
          "from afar, inside the gap, or on a key lock is refused",
          "[Locks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBanditChest);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  REQUIRE(chest.GetLock()->locked);
  picker.RemoveAllItems();
  picker.AddItem(kLockpick, 2);

  // three broke, two held: two leave, the lock stands with two broken on it
  Report(p, kBanditChest, 1, 3);
  REQUIRE(picker.GetInventory().GetItemCount(kLockpick) == 0);
  REQUIRE(chest.GetLock()->locked);
  REQUIRE(chest.GetLock()->broken == 2);

  // a second report inside the gap is refused (the picks are gone anyway)
  picker.AddItem(kLockpick, 1);
  Report(p, kBanditChest, 1, 1);
  REQUIRE(picker.GetInventory().GetItemCount(kLockpick) == 1);
  REQUIRE(chest.GetLock()->broken == 2);

  // from afar: refused
  picker.SetPos({ -3420.f, 732.f + 900.f, 575.f });
  Report(p, kDraugrChest, 0, 0);
  REQUIRE(
    p.worldState.GetFormAt<MpObjectReference>(kDraugrChest).GetLock()->locked);

  // a key lock refuses picks and opens for the key; the Lockpick stands in
  // for a key here (any base the lock names)
  MpChangeFormREFR::Lock keyed;
  keyed.level = 255;
  keyed.key = kLockpick;
  keyed.locked = true;
  chest.SetLock(keyed);
  picker.SetPos({ -3420.f, 732.f, 575.f });
  picker.RemoveAllItems();
  picker.AddItem(kLockpick, 1);
  p.Messages().clear();
  // the gap passed? the last report was moments ago: a fresh reference's
  // report is judged by reach, lock and tier before the gap, and the key
  // tier refuses it
  Report(p, kBanditChest, 0, 0);
  REQUIRE(chest.GetLock()->locked);
  // the activation with the key in the inventory opens it
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::Activate) },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", kBanditChest },
                                { "isSecondActivation", false } } } });
  REQUIRE(!chest.GetLock()->locked);
  REQUIRE(SentTo<OpenContainerMessage>(p, 0).size() == 1);
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::Activate) },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", kBanditChest },
                                { "isSecondActivation", true } } } });

  // left as found
  MpChangeFormREFR::Lock novice;
  novice.level = 0;
  novice.key = 0;
  novice.locked = true;
  chest.SetLock(novice);
  Leave(p);
}

// thuum docs/verbs/locks.md: the console's `lock <ref> [level] [key]` sets
// the level and names the key's base (the lab's key lock: 255 with a vanilla
// KEYM the console then hands the player); a level alone keeps the key, 0
// clears it; `unlock <ref>` opens. The caller's rank is the console table's
// (ConsoleCommandTest.cpp's idiom: the flag on the actor).
TEST_CASE("The console's Lock takes a level and a key, keeps the key under a "
          "level alone, and Unlock opens",
          "[Locks][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  auto& picker = p.worldState.GetFormAt<MpActor>(kPicker);
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBanditChest);
  picker.SetConsoleCommandsAllowedFlag(true);
  RawMessageData msgData;
  msgData.userId = 0;

  ConsoleCommandMessage keyed;
  keyed.data.commandName = "lock";
  keyed.data.args = { int64_t(kBanditChest), int64_t(255),
                      int64_t(kRiverwoodKey) };
  p.GetActionListener().OnConsoleCommand(msgData, keyed);
  REQUIRE(chest.GetLock().has_value());
  REQUIRE(chest.GetLock()->level == 255);
  REQUIRE(chest.GetLock()->key == kRiverwoodKey);
  REQUIRE(chest.GetLock()->locked);
  // the watcher hears the lock as a property
  REQUIRE_FALSE(LockProps(p, 1).empty());

  // a level alone keeps the key
  ConsoleCommandMessage releveled;
  releveled.data.commandName = "lock";
  releveled.data.args = { int64_t(kBanditChest), int64_t(50) };
  p.GetActionListener().OnConsoleCommand(msgData, releveled);
  REQUIRE(chest.GetLock()->level == 50);
  REQUIRE(chest.GetLock()->key == kRiverwoodKey);

  // 0 clears the key
  ConsoleCommandMessage cleared;
  cleared.data.commandName = "lock";
  cleared.data.args = { int64_t(kBanditChest), int64_t(50), int64_t(0) };
  p.GetActionListener().OnConsoleCommand(msgData, cleared);
  REQUIRE(chest.GetLock()->key == 0);
  REQUIRE(chest.GetLock()->locked);

  ConsoleCommandMessage opened;
  opened.data.commandName = "unlock";
  opened.data.args = { int64_t(kBanditChest) };
  p.GetActionListener().OnConsoleCommand(msgData, opened);
  REQUIRE_FALSE(chest.GetLock()->locked);

  // left as found
  MpChangeFormREFR::Lock novice;
  novice.level = 0;
  novice.key = 0;
  novice.locked = true;
  chest.SetLock(novice);
  picker.SetConsoleCommandsAllowedFlag(false);
  Leave(p);
}
