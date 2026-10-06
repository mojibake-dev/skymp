#include "ActionListener.h"
#include "FavoritesMessage.h"
#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

PartOne& GetPartOne();

// thuum docs/verbs/favorites.md: a player's client reports its favorites, the
// whole list, after a menu where they change closed. The server keeps the
// items the player holds and the magic, drops the rest, and replaces its
// record with what it kept; after every login it sends the record back, with
// an item sold since left out.

namespace {
constexpr uint32_t kActor = 0xff000fa5;

// Skyrim.esm, from the lab's record scans: WEAP 0x0001397E IronDagger and
// 0x00013989 SteelSword (lab/labapi guests.yaml items), SPEL 0x00012FCD
// Flames, a starting spell of the Player NPC_ 0x00000007 (SPLO; lab/esm.py,
// 2026-10-05)
constexpr uint32_t kIronDagger = 0x0001397e;
constexpr uint32_t kSteelSword = 0x00013989;
constexpr uint32_t kFlames = 0x00012fcd;
// Tamriel, a WRLD: a form that is no item and no magic
constexpr uint32_t kTamriel = 0x0000003c;

using Entries = std::vector<std::pair<uint32_t, int>>;

MpActor& Player(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, kTamriel);
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(kActor);
}

void Report(PartOne& p, const Entries& entries)
{
  RawMessageData raw;
  raw.userId = 0;
  FavoritesMessage msg;
  for (const auto& [form, hotkey] : entries) {
    msg.entries.push_back(
      FavoritesMessage::Entry{ form, static_cast<int8_t>(hotkey) });
  }
  p.GetActionListener().OnFavorites(raw, msg);
}

Entries Recorded(PartOne& p, MpActor& ac)
{
  Entries out;
  for (const auto& favorite : ac.GetFavorites()) {
    out.emplace_back(favorite.form.ToFormId(p.worldState.espmFiles),
                     favorite.hotkey);
  }
  return out;
}

// The player's own movement report, standing at the origin
void Stand(PartOne& p)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = 0;
  msg.data.pos = { 0, 0, 0 };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = kTamriel;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// The Favorites messages user 0 received since the last clear, each as its
// entries
std::vector<Entries> Sent(PartOne& p)
{
  std::vector<Entries> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::Favorites) {
      Entries entries;
      for (auto& e : m.j["entries"]) {
        entries.emplace_back(e["form"].get<uint32_t>(),
                             e["hotkey"].get<int>());
      }
      out.push_back(entries);
    }
  }
  return out;
}

void Leave(PartOne& p)
{
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A favorites report keeps held items and magic, drops the rest, "
          "and replaces the record",
          "[Favorites]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  ac.AddItem(kIronDagger, 1);

  // the steel sword is not held, Tamriel is no item, and an FF form was made
  // in a session
  Report(p,
         { { kIronDagger, 2 },
           { kFlames, -1 },
           { kSteelSword, 3 },
           { kTamriel, 4 },
           { 0xff000123, 5 } });
  REQUIRE(Recorded(p, ac) == Entries{ { kIronDagger, 2 }, { kFlames, -1 } });

  // a later report replaces it: the dagger was unmarked, Flames got key 1
  Report(p, { { kFlames, 0 } });
  REQUIRE(Recorded(p, ac) == Entries{ { kFlames, 0 } });

  // and an empty one clears it
  Report(p, {});
  REQUIRE(Recorded(p, ac).empty());

  Leave(p);
}

TEST_CASE("A login sends the favorites after the first movement, without an "
          "item sold since",
          "[Favorites]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  ac.AddItem(kIronDagger, 1);
  Report(p, { { kIronDagger, 2 }, { kFlames, 0 } });

  // nothing is sent until a login, and a login waits for the first movement
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  REQUIRE(Sent(p).empty());
  Stand(p);
  REQUIRE(Sent(p) ==
          std::vector<Entries>{ { { kIronDagger, 2 }, { kFlames, 0 } } });

  // the dagger sold, the next login leaves it out
  ac.RemoveItem(kIronDagger, 1, nullptr);
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  Stand(p);
  REQUIRE(Sent(p) == std::vector<Entries>{ { { kFlames, 0 } } });

  Leave(p);
}

TEST_CASE("The change form keeps a player's favorites, and older records "
          "read as none",
          "[Favorites]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  changeForm.favorites =
    std::vector<Favorite>{ { FormDesc::FromString("1397e:Skyrim.esm"), 2 },
                           { FormDesc::FromString("12fcd:Skyrim.esm"), -1 } };

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).favorites ==
          changeForm.favorites);

  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("favorites"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).favorites);
}
