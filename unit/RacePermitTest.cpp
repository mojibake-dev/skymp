#include "TestUtils.hpp"

#include "MsgType.h"

PartOne& GetPartOne();

// thuum docs/verbs/character-creation.md (the permit, M5): a server may
// restrict a playable race to the profiles it lists (server setting
// restrictedRaces); the lab's case is rotfern.esp's race for test client 1
// alone (profile 1). OrcRace stands in for it here: Skyrim.esm's 0x13747 has
// the Playable flag, as NordRace 0x13746 does (lab/esm.py on the lab's
// files).
TEST_CASE("A restricted race passes the race menu for a listed profile alone",
          "[PartOne][espm]")
{
  constexpr uint32_t kOrcRace = 0x13747;
  constexpr uint32_t kNordRace = 0x13746;

  auto& partOne = GetPartOne();
  partOne.SetRestrictedRacesSettings(R"({"13747:Skyrim.esm": [1]})");
  REQUIRE(partOne.worldState.RestrictedRaceIds() ==
          std::vector<uint32_t>{ kOrcRace });
  REQUIRE(partOne.worldState.PermittedRaceIds(1) ==
          std::vector<uint32_t>{ kOrcRace });
  REQUIRE(partOne.worldState.PermittedRaceIds(2).empty());

  // profile 1 on user 0, profile 2 on user 1
  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c, 1);
  partOne.Messages().clear();
  partOne.SetUserActor(0, 0xff000000);
  const auto ownCreateActor = [&](Networking::UserId user) {
    return std::find_if(
      partOne.Messages().begin(), partOne.Messages().end(), [&](auto& m) {
        return m.userId == user && m.j["t"] == MsgType::CreateActor &&
          m.j["isMe"] == true;
      });
  };
  // the player's own CreateActor names the restricted races and the ones
  // its profile may wear (the client's menu hides the difference)
  auto own = ownCreateActor(0);
  REQUIRE(own != partOne.Messages().end());
  REQUIRE(own->j["props"]["restrictedRaces"] ==
          nlohmann::json::array({ kOrcRace }));
  REQUIRE(own->j["props"]["permittedRaces"] ==
          nlohmann::json::array({ kOrcRace }));

  DoConnect(partOne, 1);
  partOne.CreateActor(0xff000001, { 100, 0, 0 }, 0, 0x3c, 2);
  partOne.Messages().clear();
  partOne.SetUserActor(1, 0xff000001);
  own = ownCreateActor(1);
  REQUIRE(own != partOne.Messages().end());
  REQUIRE(own->j["props"]["restrictedRaces"] ==
          nlohmann::json::array({ kOrcRace }));
  REQUIRE(own->j["props"]["permittedRaces"] == nlohmann::json::array());

  auto& permittedActor = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  auto& otherActor = partOne.worldState.GetFormAt<MpActor>(0xff000001);

  const auto choose = [&](Networking::UserId user, MpActor& ac,
                          uint32_t raceId) {
    auto msg = jAppearance;
    msg["idx"] = ac.GetIdx();
    msg["data"]["raceId"] = raceId;
    partOne.Messages().clear();
    DoMessage(partOne, user, msg);
  };
  const auto relayedTo = [&](Networking::UserId user) {
    return std::count_if(
      partOne.Messages().begin(), partOne.Messages().end(), [&](auto& m) {
        return m.j["t"] == MsgType::UpdateAppearance && m.userId == user;
      });
  };

  // the listed profile wears the restricted race
  partOne.SetRaceMenuOpen(0xff000000, true);
  choose(0, permittedActor, kOrcRace);
  REQUIRE(permittedActor.GetAppearance() != nullptr);
  REQUIRE(permittedActor.GetAppearance()->raceId == kOrcRace);
  REQUIRE(!permittedActor.IsRaceMenuOpen());
  REQUIRE(relayedTo(1) == 1);

  // another profile is refused it (E_APPEARANCE_RACE): the record untouched,
  // the menu still open, nothing relayed; an unrestricted playable race
  // passes as before
  partOne.SetRaceMenuOpen(0xff000001, true);
  choose(1, otherActor, kOrcRace);
  REQUIRE(otherActor.GetAppearance() == nullptr);
  REQUIRE(otherActor.IsRaceMenuOpen());
  REQUIRE(relayedTo(0) == 0);

  choose(1, otherActor, kNordRace);
  REQUIRE(otherActor.GetAppearance() != nullptr);
  REQUIRE(otherActor.GetAppearance()->raceId == kNordRace);
  REQUIRE(!otherActor.IsRaceMenuOpen());
  REQUIRE(relayedTo(0) == 1);

  // a restriction from a plugin the server does not load restricts nothing,
  // and no restriction at all leaves the menu's rule as it was
  partOne.SetRestrictedRacesSettings(R"({"aa00:NotLoaded.esp": [1]})");
  REQUIRE(partOne.worldState.RestrictedRaceIds().empty());
  partOne.SetRestrictedRacesSettings("{}");
  partOne.SetRaceMenuOpen(0xff000001, true);
  choose(1, otherActor, kOrcRace);
  REQUIRE(otherActor.GetAppearance()->raceId == kOrcRace);
  REQUIRE(!otherActor.IsRaceMenuOpen());

  REQUIRE_THROWS(partOne.SetRestrictedRacesSettings(R"({"13747": [1]})"));
  REQUIRE_THROWS(partOne.SetRestrictedRacesSettings("[]"));
  REQUIRE_THROWS(
    partOne.SetRestrictedRacesSettings(R"({"zzz:Skyrim.esm": [1]})"));
  REQUIRE_THROWS(
    partOne.SetRestrictedRacesSettings(R"({"0:Skyrim.esm": [1]})"));
  REQUIRE_THROWS(
    partOne.SetRestrictedRacesSettings(R"({"13747:Skyrim.esm": ["1"]})"));
  REQUIRE_THROWS(
    partOne.SetRestrictedRacesSettings(R"({"0800aa00:Skyrim.esm": [1]})"));
  // two spellings of one plugin are one key, their profiles merged
  partOne.SetRestrictedRacesSettings(
    R"({"13747:Skyrim.esm": [1], "13747:skyrim.esm": [2]})");
  REQUIRE(partOne.worldState.RestrictedRaceIds() ==
          std::vector<uint32_t>{ kOrcRace });
  REQUIRE(partOne.worldState.PermittedRaceIds(1) ==
          std::vector<uint32_t>{ kOrcRace });
  REQUIRE(partOne.worldState.PermittedRaceIds(2) ==
          std::vector<uint32_t>{ kOrcRace });
  partOne.SetRestrictedRacesSettings("{}");

  DoDisconnect(partOne, 0);
  DoDisconnect(partOne, 1);
  partOne.DestroyActor(0xff000000);
  partOne.DestroyActor(0xff000001);
}
