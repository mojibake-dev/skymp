#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActorValueRecord.h"
#include "CreateActorMessage.h"
#include "MsgType.h"
#include "UpdatePropertyMessage.h"

PartOne& GetPartOne();

// thuum docs/verbs/figure-values.md: another game's figure of a player
// carries the player's recorded maxima (actor-values' bases 24, 25, 26) as
// its base values, through CreateActor's health, magicka and stamina props
// for an observer, and follows a change of the record as property updates.
// Before the first record nothing travels, and the owner's own CreateActor
// keeps the race's bases as before.
namespace {
constexpr uint32_t kSeen = 0xff000200;
constexpr uint32_t kObserver = 0xff000201;

// the CreateActor messages user `userId` received for `refrId`
std::vector<CreateActorMessage> SeenBy(PartOne& p, Networking::UserId userId,
                                       uint32_t refrId)
{
  std::vector<CreateActorMessage> out;
  auto found = FindRefrMessage<CreateActorMessage>(p, refrId);
  for (size_t i = 0; i < found.filteredMessages.size(); ++i) {
    if (found.filteredMessagesOriginals[i].userId == userId) {
      out.push_back(found.filteredMessages[i]);
    }
  }
  return out;
}

// the property updates named `prop` that user `userId` received
std::vector<std::string> PropertyDumps(PartOne& p, Networking::UserId userId,
                                       const char* prop)
{
  std::vector<std::string> out;
  for (auto& m : p.Messages()) {
    if (m.userId != userId) {
      continue;
    }
    if (auto upd = dynamic_cast<UpdatePropertyMessage*>(m.message.get())) {
      if (upd->propName == prop) {
        out.push_back(upd->dataDump);
      }
    }
  }
  return out;
}
}

TEST_CASE("A figure carries its player's recorded maxima, and follows them",
          "[FigureValues]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kSeen, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kSeen);
  p.CreateActor(kObserver, { 0, 100, 0 }, 0, 0x3c);
  p.SetUserActor(1, kObserver);

  // no record yet: the observer's CreateActor of the seen actor carries no
  // maxima (the figure keeps the engine's defaults); the owner's own carries
  // the race's bases as always
  auto first = SeenBy(p, 1, kSeen);
  REQUIRE(first.size() >= 1);
  REQUIRE_FALSE(first.back().props.health.has_value());
  REQUIRE_FALSE(first.back().props.magicka.has_value());
  REQUIRE_FALSE(first.back().props.stamina.has_value());
  auto own = SeenBy(p, 0, kSeen);
  REQUIRE(own.size() >= 1);
  REQUIRE(own.back().props.health == 100.f);

  // the record arrives (actor-values' report): the observer gets the three
  // maxima as property updates
  auto& seen = p.worldState.GetFormAt<MpActor>(kSeen);
  ActorValueRecord record;
  record.bases = { { 24, 250.f }, { 25, 120.f }, { 26, 130.f } };
  REQUIRE(seen.SetActorValueRecord(record));
  const size_t before = p.Messages().size();
  seen.SendMaximaToListeners();
  REQUIRE(p.Messages().size() > before);
  auto healthDumps = PropertyDumps(p, 1, "health");
  REQUIRE(healthDumps.size() == 1);
  REQUIRE(std::stof(healthDumps.back()) == Catch::Approx(250.f));
  REQUIRE(std::stof(PropertyDumps(p, 1, "magicka").back()) ==
          Catch::Approx(120.f));
  REQUIRE(std::stof(PropertyDumps(p, 1, "stamina").back()) ==
          Catch::Approx(130.f));
  // the owner is not told its own maxima this way
  REQUIRE(PropertyDumps(p, 0, "health").empty());

  // a later spawn of the figure carries them in CreateActor itself
  p.DestroyActor(kObserver);
  p.CreateActor(kObserver, { 0, 100, 0 }, 0, 0x3c);
  p.SetUserActor(1, kObserver);
  auto later = SeenBy(p, 1, kSeen);
  REQUIRE(later.size() >= 2);
  REQUIRE(later.back().props.health == Catch::Approx(250.f));
  REQUIRE(later.back().props.magicka == Catch::Approx(120.f));
  REQUIRE(later.back().props.stamina == Catch::Approx(130.f));

  p.DestroyActor(kObserver);
  p.DestroyActor(kSeen);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
