#include "../skyrim-platform/src/platform_se/skyrim_platform/OwnedObject.h"
#include <catch2/catch_all.hpp>

// A world object the host's game owns, its script run there for a guest's
// press (OwnedObject.h, OnCall's rules). The host's engine accepts a graph
// event on an object its own player is away from but does not advance the
// graph until that player comes near, so a pillar's PlayAnimationAndWait
// would wait until then; the guest's game renders the object. The header is
// std only, so the server's T0 build reaches it by path, as
// PushRouteTest.cpp reaches PushRoute.h
TEST_CASE("The host answers a pillar's turn a guest pressed at once, its 3D "
          "loaded or not",
          "[OwnedObject]")
{
  REQUIRE(OwnedObject::AnswersAtOnce(true, false, true, true));
  REQUIRE(OwnedObject::AnswersAtOnce(true, false, false, true));
}

TEST_CASE("A cobweb's own tear sound on the host crosses to the games that "
          "render the web",
          "[OwnedObject]")
{
  REQUIRE(OwnedObject::OwnLookCrosses(true, true));
}
