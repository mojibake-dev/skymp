#include "TestUtils.hpp"

PartOne& GetPartOne();

// thuum: a host attempt on a form no movement was recorded for wrote past
// the end of WorldState::lastMovUpdateByIdx and aborted the server on
// libstdc++'s bounds assertion. The Apocalypse layer's sweep met it in
// 20261016-233122-a-movement-speed's server.log: a client's game connected
// to a fresh server, asked to host before its actor was attached, logged in,
// and its next host attempt brought the server down.
TEST_CASE("Hosting an actor no movement was recorded for", "[PartOne]")
{
  auto& partOne = GetPartOne();
  const auto saved = partOne.worldState.lastMovUpdateByIdx;
  partOne.worldState.lastMovUpdateByIdx.clear();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);
  partOne.CreateActor(0xff000001, { 100, 0, 0 }, 0, 0x3c);

  const auto remoteIdx =
    partOne.worldState.GetFormAt<MpObjectReference>(0xff000001).GetIdx();

  DoMessage(
    partOne, 0,
    nlohmann::json{ { "t", MsgType::Host }, { "remoteId", 0xff000001 } });

  REQUIRE(partOne.worldState.hosters[0xff000001] == 0xff000000);
  REQUIRE(partOne.worldState.lastMovUpdateByIdx.size() > remoteIdx);
  REQUIRE(partOne.worldState.lastMovUpdateByIdx[remoteIdx].has_value());

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
  partOne.DestroyActor(0xff000001);
  partOne.worldState.hosters.erase(0xff000001);
  partOne.worldState.lastMovUpdateByIdx = saved;
}
