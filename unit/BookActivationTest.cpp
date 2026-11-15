#include "MpActor.h"
#include "MpObjectReference.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include <algorithm>
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum playtest 52 break 3 (lab/.cache/drafts/reads-53/r-rw-and-r-b.md, R-B):
// a world book's "Read" put the book in the bag, since the server picked up
// any item at its activation. A player's activation of a world book is now
// answered with the open (its game runs the engine's own activation, whose
// Book Menu is that game's alone) and takes nothing; its next activation of
// that book is the menu's Take, the pickup as it always was (HYPOTHESIS: the
// shape the client half reports the Take in, unbuilt until x-book-take). A
// spell tome, which the engine picks up in the world, and every activator
// but a player's own game keep the pickup. The game files as lab/esm.py read
// Skyrim.esm (2026-11-14): MorthalMoorsideInn 0x138ce holds REFR 0x10544d of
// BOOK 0xed63f Book2CommonWatcherofStones at (943.6, -723.3, 74.6) and REFR
// 0x10544e of BOOK 0xed033 Book2CommonFallFromGlory at (946.3, -716.2, 74.3),
// both DATA flags 0x00, beside PeltFloor02 0x0e00f0 at (1061, -721, 1); the
// one world spell tome in Tamriel 0x3c is REFR 0xef5ea of BOOK 0xa2703
// SpellTomeFlameCloak (DATA flags 0x04, teaches a spell) at (60786.8,
// 104299.8, -13983.7).
namespace {
constexpr uint32_t kInn = 0x138ce;
constexpr uint32_t kWatcher = 0x10544d;
constexpr uint32_t kWatcherBase = 0xed63f;
constexpr uint32_t kGlory = 0x10544e;
constexpr uint32_t kGloryBase = 0xed033;
constexpr uint32_t kTome = 0xef5ea;
constexpr uint32_t kTomeBase = 0xa2703;
constexpr uint32_t kReader = 0xff000fb1;
constexpr uint32_t kOther = 0xff000fb2;
constexpr uint32_t kNobody = 0xff000fb3;
// on the rug, 116 to 140 units from the books: inside the activation reach
const NiPoint3 kOnTheRug{ 1060.f, -722.f, 0.f };

// the player's own activation, as its game sends it
void ActivateAsPlayer(PartOne& p, Networking::UserId user, uint32_t target,
                      bool isSecondActivation = false)
{
  DoMessage(
    p, user,
    nlohmann::json{ { "t", MsgType::Activate },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", target },
                        { "isSecondActivation", isSecondActivation } } } });
  p.Tick(); // the deferred inventory
}

// the open: the reply that makes the player's game activate the reference
size_t OpensSentTo(PartOne& p, Networking::UserId user, uint32_t target)
{
  return std::count_if(
    p.Messages().begin(), p.Messages().end(), [&](PartOne::Message& m) {
      return m.userId == user && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::OpenContainer) &&
        m.j.contains("target") && m.j["target"] == target;
    });
}

bool InventorySentTo(PartOne& p, Networking::UserId user)
{
  return std::any_of(
    p.Messages().begin(), p.Messages().end(), [&](PartOne::Message& m) {
      return m.userId == user && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::SetInventory);
    });
}

// a player (user `user`) on the inn's rug, its bag empty
MpActor& EnterInn(PartOne& p, Networking::UserId user, uint32_t actorId,
                  const NiPoint3& at = kOnTheRug)
{
  DoConnect(p, user);
  p.CreateActor(actorId, at, 0, kInn);
  p.SetUserActor(user, actorId);
  auto& actor = p.worldState.GetFormAt<MpActor>(actorId);
  actor.RemoveAllItems();
  return actor;
}

void Leave(PartOne& p, Networking::UserId user, uint32_t actorId)
{
  DoDisconnect(p, user);
  p.DestroyActor(actorId);
}
}

TEST_CASE("A player's activation of a world book opens it in its own game "
          "and takes nothing; its next activation of that book takes it",
          "[Books][PartOne][espm]")
{
  auto& p = GetPartOne();
  auto& book = p.worldState.GetFormAt<MpObjectReference>(kWatcher);
  REQUIRE(book.GetBaseId() == kWatcherBase);
  auto& reader = EnterInn(p, 0, kReader);
  p.Messages().clear();

  ActivateAsPlayer(p, 0, kWatcher);
  REQUIRE(OpensSentTo(p, 0, kWatcher) == 1);
  REQUIRE(!InventorySentTo(p, 0));
  REQUIRE(!book.IsHarvested());
  REQUIRE(reader.GetInventory().GetItemCount(kWatcherBase) == 0);
  REQUIRE(reader.GetOpenBook() == kWatcher);

  // the Take: the pickup as it always was, and no second open
  p.Messages().clear();
  ActivateAsPlayer(p, 0, kWatcher);
  REQUIRE(OpensSentTo(p, 0, kWatcher) == 0);
  REQUIRE(InventorySentTo(p, 0));
  REQUIRE(book.IsHarvested());
  REQUIRE(reader.GetInventory().GetItemCount(kWatcherBase) == 1);
  REQUIRE(reader.GetOpenBook() == 0);

  // taken: nothing to open and nothing more to take
  p.Messages().clear();
  ActivateAsPlayer(p, 0, kWatcher);
  REQUIRE(OpensSentTo(p, 0, kWatcher) == 0);
  REQUIRE(reader.GetInventory().GetItemCount(kWatcherBase) == 1);
  REQUIRE(reader.GetOpenBook() == 0);

  Leave(p, 0, kReader);
}

TEST_CASE("The Book Menu's closing activation forgets the open: the next "
          "activation opens the book again and takes nothing",
          "[Books][PartOne][espm]")
{
  auto& p = GetPartOne();
  auto& book = p.worldState.GetFormAt<MpObjectReference>(kWatcher);
  auto& reader = EnterInn(p, 0, kReader);

  ActivateAsPlayer(p, 0, kWatcher);
  REQUIRE(reader.GetOpenBook() == kWatcher);

  p.Messages().clear();
  ActivateAsPlayer(p, 0, kWatcher, true);
  REQUIRE(reader.GetOpenBook() == 0);
  REQUIRE(!InventorySentTo(p, 0));
  REQUIRE(!book.IsHarvested());

  p.Messages().clear();
  ActivateAsPlayer(p, 0, kWatcher);
  REQUIRE(OpensSentTo(p, 0, kWatcher) == 1);
  REQUIRE(!book.IsHarvested());
  REQUIRE(reader.GetInventory().GetItemCount(kWatcherBase) == 0);
  REQUIRE(reader.GetOpenBook() == kWatcher);

  Leave(p, 0, kReader);
}

TEST_CASE("A player's activation of anything else forgets its open book, so "
          "coming back to the book opens it again",
          "[Books][PartOne][espm]")
{
  auto& p = GetPartOne();
  auto& watcher = p.worldState.GetFormAt<MpObjectReference>(kWatcher);
  auto& glory = p.worldState.GetFormAt<MpObjectReference>(kGlory);
  REQUIRE(glory.GetBaseId() == kGloryBase);
  auto& reader = EnterInn(p, 0, kReader);

  ActivateAsPlayer(p, 0, kWatcher);
  REQUIRE(reader.GetOpenBook() == kWatcher);

  p.Messages().clear();
  ActivateAsPlayer(p, 0, kGlory);
  REQUIRE(OpensSentTo(p, 0, kGlory) == 1);
  REQUIRE(reader.GetOpenBook() == kGlory);

  p.Messages().clear();
  ActivateAsPlayer(p, 0, kWatcher);
  REQUIRE(OpensSentTo(p, 0, kWatcher) == 1);
  REQUIRE(reader.GetOpenBook() == kWatcher);
  REQUIRE(!watcher.IsHarvested());
  REQUIRE(!glory.IsHarvested());
  REQUIRE(reader.GetInventory().GetItemCount(kWatcherBase) == 0);
  REQUIRE(reader.GetInventory().GetItemCount(kGloryBase) == 0);

  Leave(p, 0, kReader);
}

TEST_CASE("Two players read one book: the first take wins and the other's "
          "take finds it taken",
          "[Books][PartOne][espm]")
{
  auto& p = GetPartOne();
  auto& book = p.worldState.GetFormAt<MpObjectReference>(kWatcher);
  auto& reader = EnterInn(p, 0, kReader);
  auto& other =
    EnterInn(p, 1, kOther, kOnTheRug + NiPoint3{ 0.f, -40.f, 0.f });

  ActivateAsPlayer(p, 0, kWatcher);
  ActivateAsPlayer(p, 1, kWatcher);
  REQUIRE(reader.GetOpenBook() == kWatcher);
  REQUIRE(other.GetOpenBook() == kWatcher);
  REQUIRE(!book.IsHarvested());

  ActivateAsPlayer(p, 1, kWatcher);
  REQUIRE(book.IsHarvested());
  REQUIRE(other.GetInventory().GetItemCount(kWatcherBase) == 1);

  p.Messages().clear();
  ActivateAsPlayer(p, 0, kWatcher);
  REQUIRE(OpensSentTo(p, 0, kWatcher) == 0);
  REQUIRE(reader.GetInventory().GetItemCount(kWatcherBase) == 0);
  REQUIRE(reader.GetOpenBook() == 0);
  REQUIRE(other.GetInventory().GetItemCount(kWatcherBase) == 1);

  Leave(p, 0, kReader);
  Leave(p, 1, kOther);
}

TEST_CASE("A disconnect forgets the player's open book",
          "[Books][PartOne][espm]")
{
  auto& p = GetPartOne();
  auto& reader = EnterInn(p, 0, kReader);

  ActivateAsPlayer(p, 0, kWatcher);
  REQUIRE(reader.GetOpenBook() == kWatcher);

  DoDisconnect(p, 0);
  REQUIRE(reader.GetOpenBook() == 0);
  p.DestroyActor(kReader);
}

TEST_CASE("A player's activation of a spell tome picks it up at once, as the "
          "engine does",
          "[Books][PartOne][espm]")
{
  auto& p = GetPartOne();
  auto& tome = p.worldState.GetFormAt<MpObjectReference>(kTome);
  REQUIRE(tome.GetBaseId() == kTomeBase);
  DoConnect(p, 0);
  p.CreateActor(kReader, tome.GetPos() + NiPoint3{ 60.f, 0.f, 0.f }, 0,
                ToUnderlying(Constants::kTamriel));
  p.SetUserActor(0, kReader);
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  reader.RemoveAllItems();
  p.Messages().clear();

  ActivateAsPlayer(p, 0, kTome);
  REQUIRE(OpensSentTo(p, 0, kTome) == 0);
  REQUIRE(InventorySentTo(p, 0));
  REQUIRE(tome.IsHarvested());
  REQUIRE(reader.GetInventory().GetItemCount(kTomeBase) == 1);
  REQUIRE(reader.GetOpenBook() == 0);

  Leave(p, 0, kReader);
}

TEST_CASE("An activation of a world book by anything but a player's own game "
          "picks it up at once, as before",
          "[Books][PartOne][espm]")
{
  auto& p = GetPartOne();
  auto& watcher = p.worldState.GetFormAt<MpObjectReference>(kWatcher);
  auto& glory = p.worldState.GetFormAt<MpObjectReference>(kGlory);

  // an actor no user plays (PickUpItemCountTest's shape)
  p.worldState.AddForm(
    std::make_unique<MpActor>(
      LocationalData{ kOnTheRug, NiPoint3(), watcher.GetCellOrWorld() },
      FormCallbacks::DoNothing()),
    kNobody);
  auto& nobody = p.worldState.GetFormAt<MpActor>(kNobody);
  nobody.RemoveAllItems();
  p.Messages().clear();

  watcher.Activate(nobody);
  REQUIRE(watcher.IsHarvested());
  REQUIRE(nobody.GetInventory().GetItemCount(kWatcherBase) == 1);

  // and a host's report of its NPC's activation
  constexpr bool kDefaultProcessingOnly = false;
  constexpr bool kSecondActivation = false;
  constexpr bool kReportedByHost = true;
  glory.Activate(nobody, kDefaultProcessingOnly, kSecondActivation,
                 kReportedByHost);
  REQUIRE(glory.IsHarvested());
  REQUIRE(nobody.GetInventory().GetItemCount(kGloryBase) == 1);

  REQUIRE(std::none_of(
    p.Messages().begin(), p.Messages().end(), [](PartOne::Message& m) {
      return m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::OpenContainer);
    }));
  REQUIRE(nobody.GetOpenBook() == 0);
}
