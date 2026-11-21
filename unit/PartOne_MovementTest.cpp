#include "TestUtils.hpp"

TEST_CASE("Hypothesis: UpdateMovement may send nothing when actor without "
          "user present",
          "[PartOne]")
{

  PartOne partOne;

  constexpr uint32_t n = 20;
  static_assert(n <= MAX_PLAYERS - 1);

  for (uint32_t i = 0; i < n; ++i) {
    partOne.CreateActor(i + 0xff000000, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
    if (i % 2 == 0)
      continue;

    DoConnect(partOne, i + 1);
    partOne.SetUserActor(i + 1, i + 0xff000000);

    DoUpdateMovement(partOne, i + 0xff000000, i + 1);
  }

  DoConnect(partOne, 0);
  partOne.CreateActor(0xffffffff, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
  partOne.SetUserActor(0, 0xffffffff);
  partOne.Messages().clear();

  DoUpdateMovement(partOne, 0xffffffff, 0);
  REQUIRE(partOne.Messages().size() ==
          11); // Me and 10 other users created in loop
}

TEST_CASE("UpdateMovement when neighbour has been disconnected", "[PartOne]")
{

  PartOne partOne;

  for (int i = 0; i < 2; ++i) {
    DoConnect(partOne, i);
    partOne.CreateActor(i + 0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
    partOne.SetUserActor(i, i + 0xff000ABC);
    auto m = jMovement;
    m["idx"] = i;
    DoMessage(partOne, i, m);
  }

  partOne.Messages().clear();

  DoDisconnect(partOne, 1);

  partOne.Messages().clear();
  DoMessage(partOne, 0, jMovement);
  REQUIRE(partOne.Messages().size() == 1);
}

TEST_CASE("UpdateMovement", "[PartOne]")
{

  PartOne partOne;

  auto doMovement = [&] { DoMessage(partOne, 0, jMovement); };

  DoConnect(partOne, 0);

  doMovement();
  REQUIRE(partOne.Messages().size() == 0); // No actor - no movement

  partOne.CreateActor(0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
  partOne.SetUserActor(0, 0xff000ABC);
  partOne.Messages().clear();
  doMovement();
  REQUIRE(partOne.Messages().size() == 1);
  REQUIRE(partOne.Messages().at(0).j == jMovement);

  // UpdateMovement actually changes position and rotation
  REQUIRE(
    dynamic_cast<MpActor*>(partOne.worldState.LookupFormById(0xff000ABC).get())
      ->GetPos() == NiPoint3{ 1, -1, 1 });
  REQUIRE(
    dynamic_cast<MpActor*>(partOne.worldState.LookupFormById(0xff000ABC).get())
      ->GetAngle() == NiPoint3{ 0, 0, 179 });

  // Another player connects and see us
  DoConnect(partOne, 1);
  partOne.CreateActor(0xff00ABCD, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
  partOne.Messages().clear();
  partOne.SetUserActor(1, 0xff00ABCD);
  REQUIRE(partOne.Messages().size() == 3);
  // Create idx 1 for user 0, then idx 0 for 1, then idx 1 for 1 (self
  // streaming)
  REQUIRE(std::find_if(partOne.Messages().begin(), partOne.Messages().end(),
                       [&](auto m) {
                         return m.j["t"] == MsgType::CreateActor &&
                           m.j["idx"] == 1 && m.reliable && m.userId == 0;
                       }) != partOne.Messages().end());
  REQUIRE(std::find_if(partOne.Messages().begin(), partOne.Messages().end(),
                       [&](auto m) {
                         return m.j["t"] == MsgType::CreateActor &&
                           m.j["idx"] == 0 && m.reliable && m.userId == 1;
                       }) != partOne.Messages().end());
  REQUIRE(std::find_if(partOne.Messages().begin(), partOne.Messages().end(),
                       [&](auto m) {
                         return m.j["t"] == MsgType::CreateActor &&
                           m.j["idx"] == 1 && m.reliable && m.userId == 1;
                       }) != partOne.Messages().end());

  // Appearance must be empty by default
  REQUIRE(std::find_if(partOne.Messages().begin(), partOne.Messages().end(),
                       [&](auto m) { return m.j["appearance"] != nullptr; }) ==
          partOne.Messages().end());

  partOne.Messages().clear();
  doMovement();
  REQUIRE(partOne.Messages().size() == 2);
  REQUIRE(partOne.Messages().at(0).j == jMovement);
  REQUIRE(partOne.Messages().at(1).j == jMovement);

  // Another player is being moved away and now doesn't see our movement
  auto acAbcd = dynamic_cast<MpActor*>(
    partOne.worldState.LookupFormById(0xff00ABCD).get());
  partOne.Messages().clear();
  acAbcd->SetPos({ 100000, 0, 0 });
  REQUIRE(partOne.Messages().size() == 2);
  REQUIRE(std::find_if(partOne.Messages().begin(), partOne.Messages().end(),
                       [&](auto m) {
                         return m.j["t"] == MsgType::DestroyActor &&
                           m.j["idx"] == 1 && m.reliable && m.userId == 0;
                       }) != partOne.Messages().end());
  REQUIRE(std::find_if(partOne.Messages().begin(), partOne.Messages().end(),
                       [&](auto m) {
                         return m.j["t"] == MsgType::DestroyActor &&
                           m.j["idx"] == 0 && m.reliable && m.userId == 1;
                       }) != partOne.Messages().end());
}

TEST_CASE("A movement the server rejects reaches no neighbour", "[PartOne]")
{
  // thuum docs/verbs/validation.md, Movement: validate, then relay. Since
  // ADR-079 decision 7 (tracker #191) a player's own move is never sent
  // back; what the server still drops is a report sent before a teleport it
  // made itself (docs/verbs/movement-speed.md, arrivals), with nothing sent
  // back and nothing relayed
  PartOne partOne;

  for (int i = 0; i < 2; ++i) {
    DoConnect(partOne, i);
    partOne.CreateActor(i + 0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
    partOne.SetUserActor(i, i + 0xff000ABC);
    auto m = jMovement;
    m["idx"] = i;
    DoMessage(partOne, i, m);
  }
  auto& mover = partOne.worldState.GetFormAt<MpActor>(0xff000ABC);

  // the server moves user 0's actor 800 units east; its game's next report
  // comes from where it stood before
  mover.Teleport(
    LocationalData{ { 800.f, -1.f, 1.f }, { 0, 0, 0 }, FormDesc::Tamriel() });
  partOne.Messages().clear();
  auto stale = jMovement;
  stale["idx"] = 0;
  stale["data"]["pos"] = { 10.f, -1.f, 1.f };
  DoMessage(partOne, 0, stale);

  bool relayed = false;
  bool snappedBack = false;
  for (auto& m : partOne.Messages()) {
    const int t = m.j.value("t", 0);
    if (t == static_cast<int>(MsgType::UpdateMovement)) {
      relayed = true;
    }
    if (m.userId == 0 && t == static_cast<int>(MsgType::Teleport2)) {
      snappedBack = true;
    }
  }
  REQUIRE_FALSE(snappedBack);
  REQUIRE_FALSE(relayed);
  REQUIRE(mover.GetPos() == NiPoint3{ 800.f, -1.f, 1.f });
}

// thuum ADR-079 decision 7 (Eli, 2026-11-20, tracker #191): the player's own
// game moving it, by a jump in its worldspace (fast travel, a carriage) or
// into another cell (a jail, a Black Book's Apocrypha, a script's MoveTo),
// is the record's: the sender is never sent back, the move reaches its
// neighbour, placed there rather than glided
TEST_CASE("A player's own step past its budget, jump and cell change are "
          "recorded and relayed, never sent back",
          "[PartOne]")
{
  PartOne partOne;

  for (int i = 0; i < 2; ++i) {
    DoConnect(partOne, i);
    partOne.CreateActor(i + 0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
    partOne.SetUserActor(i, i + 0xff000ABC);
    auto m = jMovement;
    m["idx"] = i;
    DoMessage(partOne, i, m);
  }
  auto& mover = partOne.worldState.GetFormAt<MpActor>(0xff000ABC);
  const auto count = [&](Networking::UserId user, MsgType type) {
    int n = 0;
    for (auto& m : partOne.Messages()) {
      n += m.userId == user && m.j.value("t", 0) == static_cast<int>(type);
    }
    return n;
  };

  // a step of 3000 units within the cell: past the ground budget's burst
  // (2048), short of a jump (a script's MoveTo)
  partOne.Messages().clear();
  auto step = jMovement;
  step["idx"] = 0;
  step["data"]["pos"] = { 3001.f, -1.f, 1.f };
  DoMessage(partOne, 0, step);
  REQUIRE(count(0, MsgType::Teleport2) == 0);
  REQUIRE(count(1, MsgType::UpdateMovement) == 1);
  REQUIRE(count(1, MsgType::ActorJump) == 1);
  REQUIRE(mover.GetPos() == NiPoint3{ 3001.f, -1.f, 1.f });

  // a jump of 5000 units more in Tamriel
  partOne.Messages().clear();
  auto jump = jMovement;
  jump["idx"] = 0;
  jump["data"]["pos"] = { 8001.f, -1.f, 1.f };
  DoMessage(partOne, 0, jump);
  REQUIRE(count(0, MsgType::Teleport2) == 0);
  REQUIRE(count(1, MsgType::ActorJump) <= 1);
  REQUIRE(mover.GetPos() == NiPoint3{ 8001.f, -1.f, 1.f });

  // into an interior cell (Bleak Falls Temple's id; this world loads no
  // master file, so any cell id stands)
  partOne.Messages().clear();
  auto inside = jMovement;
  inside["idx"] = 0;
  inside["data"]["worldOrCell"] = 0x371de;
  inside["data"]["pos"] = { 100.f, 200.f, 0.f };
  DoMessage(partOne, 0, inside);
  REQUIRE(count(0, MsgType::Teleport2) == 0);
  REQUIRE(mover.GetCellOrWorld().ToFormId(partOne.worldState.espmFiles) ==
          0x371de);
  REQUIRE(mover.GetPos() == NiPoint3{ 100.f, 200.f, 0.f });
}
