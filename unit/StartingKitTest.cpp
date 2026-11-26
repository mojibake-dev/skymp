#include "ActionListener.h"
#include "RawMessageData.h"
#include "TestUtils.hpp"
#include "UpdateAppearanceMessage.h"
#include "UpdateMovementMessage.h"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

namespace {
// AbandonedPrison01 (Skyrim.esm CELL 0x21594), where a new character
// wakes: the start marker's spot
constexpr uint32_t kPrisonCell = 0x21594;
const NiPoint3 kPrisonMarker{ 6481.f, 1334.f, 191.f };
// ClothesPrisonerTunic and ClothesPrisonerShoes (Skyrim.esm), and gold
constexpr uint32_t kTunic = 0x3c9fe;
constexpr uint32_t kShoes = 0x3ca00;
constexpr uint32_t kGold = 0xf;

constexpr const char* kKit = R"([
  {"form": "03c9fe:Skyrim.esm", "count": 1, "worn": true},
  {"form": "03ca00:Skyrim.esm", "count": 1, "worn": true},
  {"form": "00000f:Skyrim.esm", "count": 5, "worn": false}
])";

// the player's own movement report from a cell or worldspace
void Report(PartOne& p, MpActor& me, uint32_t worldOrCell, const NiPoint3& pos)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = me.GetIdx();
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = worldOrCell;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// the player's appearance report, which closes the race menu
void Dress(PartOne& p, Networking::UserId user)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateAppearanceMessage msg;
  msg.idx = 0;
  msg.data = Appearance();
  msg.data->raceId = 0x00013746; // NordRace
  p.GetActionListener().OnUpdateAppearance(raw, msg);
}

// the Quest.SetStage snippets user 0 was sent: each quest and its stage
std::vector<std::pair<uint32_t, double>> StagesTold(PartOne& p)
{
  std::vector<std::pair<uint32_t, double>> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::SpSnippet &&
        m.j["class"] == "Quest" && m.j["function"] == "SetStage" &&
        m.j["arguments"].size() == 1) {
      out.emplace_back(m.j["selfId"].get<uint32_t>(),
                       m.j["arguments"][0].get<double>());
    }
  }
  return out;
}

uint32_t Count(const Inventory& inv, uint32_t baseId)
{
  uint32_t n = 0;
  for (const auto& e : inv.entries) {
    n += e.baseId == baseId ? e.count : 0;
  }
  return n;
}

bool Worn(const MpActor& ac, uint32_t baseId)
{
  for (const auto& e : ac.GetEquipment().inv.entries) {
    if (e.baseId == baseId && e.GetWorn() != Inventory::Worn::None) {
      return true;
    }
  }
  return false;
}
}

TEST_CASE("A new player's character starts in the server's starting kit, "
          "its clothes worn",
          "[StartingKit][espm]")
{
  PartOne& p = GetPartOne();
  p.SetStartingKitSettings(kKit);

  p.CreateActor(0xff000000, kPrisonMarker, 0, kPrisonCell, 9);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  REQUIRE(Count(ac.GetInventory(), kTunic) == 1);
  REQUIRE(Count(ac.GetInventory(), kShoes) == 1);
  REQUIRE(Count(ac.GetInventory(), kGold) == 5);
  REQUIRE(Worn(ac, kTunic));
  REQUIRE(Worn(ac, kShoes));
  REQUIRE_FALSE(Worn(ac, kGold));

  p.DestroyActor(0xff000000);
  p.SetStartingKitSettings("[]");
}

TEST_CASE("A new player's first move out of the prison to the start it "
          "chose is recorded as its own move, never sent back",
          "[StartingKit][espm]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(0xff000000, kPrisonMarker, 0, kPrisonCell, 9);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  const auto sentBack = [&] {
    int n = 0;
    for (auto& m : p.Messages()) {
      n += m.userId == 0 && m.j["t"] == MsgType::Teleport2;
    }
    return n;
  };

  Report(p, ac, kPrisonCell, kPrisonMarker);
  p.Messages().clear();
  // the start's own MoveTo, to Riverwood in Tamriel
  Report(p, ac, 0x3c, { 18432, -47104, 500 });
  REQUIRE(sentBack() == 0);
  REQUIRE(ac.GetCellOrWorld() == FormDesc::Tamriel());
  REQUIRE(ac.GetPos() == NiPoint3{ 18432, -47104, 500 });

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("A new character's first race menu close sets each new-player "
          "quest's stage in its own game, and a later close sets none",
          "[StartingKit][espm]")
{
  PartOne& p = GetPartOne();
  // MQ101 (Skyrim.esm), stage 10
  p.SetNewPlayerQuestsSettings(
    R"([{"quest": "03372b:Skyrim.esm", "stage": 10}])");
  DoConnect(p, 0);
  p.CreateActor(0xff000000, kPrisonMarker, 0, kPrisonCell, 9);
  p.SetUserActor(0, 0xff000000);
  p.SetRaceMenuOpen(0xff000000, true);
  p.Messages().clear();

  Dress(p, 0);
  p.Tick(); // snippets are deferred
  const auto first = StagesTold(p);
  REQUIRE(first.size() == 1);
  REQUIRE(first[0].first == 0x3372b);
  REQUIRE(first[0].second == 10.0);

  // a race menu opened again later: the character is not new
  p.SetRaceMenuOpen(0xff000000, true);
  p.Messages().clear();
  Dress(p, 0);
  p.Tick();
  REQUIRE(StagesTold(p).empty());

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
  p.SetNewPlayerQuestsSettings("[]");
}
