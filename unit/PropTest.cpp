#include "ActionListener.h"
#include "PropClaimMessage.h"
#include "PropOwnerMessage.h"
#include "PropTransformMessage.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum docs/verbs/prop-physics.md (ADR-058): one simulating seat per physics
// prop. The cell's host simulates a prop by default; the seat whose player
// touched it takes it (the claim, R0, newest touch wins at the next epoch)
// and every listener of the prop hears the word (PropOwner); only the
// owner's transforms move the record (R2) and reach the prop's other
// listeners, unreliable while the body is awake and reliable at its sleep;
// a toucher that leaves hands its props back to the host. The lease's end
// (PROP_LEASE_MS after the sleep) is the Rust rule's test (wire-rules
// props.rs): the clock cannot be advanced from here.

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kC1 = 0xff000d01;
constexpr uint32_t kC2 = 0xff000d02;
constexpr uint32_t kBasket = 0xff000d03;
constexpr uint32_t kC3 = 0xff000d04;
constexpr uint32_t kChest = 0xff000d05;
// BarrelFood01_NoRespawn, a CONT of Skyrim.esm (esm.py): no prop
constexpr uint32_t kBarrelFood01NoRespawn = 0x10e882;
// Bucket01, a MISC of Skyrim.esm (the one Riverwood's 0x2ad9c is placed from)
constexpr uint32_t kBucket01 = 0x12fdf;
// the exterior grid square (2, 0) of Tamriel (CellHostTest's kHere), and the
// next square east, beyond the claimant's reach
const NiPoint3 kHere{ 9000.f, 500.f, 0.f };
const NiPoint3 kEast{ 13500.f, 500.f, 0.f };

MpActor& Player(PartOne& p, Networking::UserId user, uint32_t id,
                const NiPoint3& pos)
{
  DoConnect(p, user);
  p.CreateActor(id, pos, 0, kTamriel);
  p.SetUserActor(user, id);
  return p.worldState.GetFormAt<MpActor>(id);
}

void CellLoaded(PartOne& p, Networking::UserId user, uint32_t cell)
{
  DoMessage(p, user,
            nlohmann::json{ { "t", MsgType::CellLoaded },
                            { "cell", cell },
                            { "loaded", true } });
}

// a server-made item reference at `pos`, the shape a drop takes
// (MpActor::DropItem's PlaceAtMe): a prop from its first frame
MpObjectReference& Prop(PartOne& p, uint32_t id, const NiPoint3& pos)
{
  auto refr = std::make_unique<MpObjectReference>(
    LocationalData{ pos, { 0.f, 0.f, 0.f }, FormDesc::Tamriel() },
    p.CreateFormCallbacks(), kBucket01, "MISC");
  p.worldState.AddForm(std::move(refr), id);
  auto& prop = p.worldState.GetFormAt<MpObjectReference>(id);
  prop.ForceSubscriptionsUpdate();
  return prop;
}

static uint8_t g_unparsed[] = { Networking::MinPacketId, '{', '}' };

RawMessageData Raw(Networking::UserId user)
{
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = g_unparsed;
  raw.unparsedLength = sizeof(g_unparsed);
  return raw;
}

// `user`'s touch of the prop `refr`
void Claim(PartOne& p, Networking::UserId user, uint32_t refr, uint32_t touch)
{
  PropClaimMessage msg;
  msg.refr = refr;
  msg.touch = touch;
  p.GetActionListener().OnPropClaim(Raw(user), msg);
}

// `user`'s sample of the prop `refr`
void Sample(PartOne& p, Networking::UserId user, uint32_t refr,
            const NiPoint3& pos, bool asleep)
{
  PropTransformMessage msg;
  msg.refr = refr;
  msg.pos = { pos.x, pos.y, pos.z };
  msg.rot = { 0.f, 0.f, 72.f };
  msg.asleep = asleep;
  p.GetActionListener().OnPropTransform(Raw(user), msg);
}

template <class Message>
std::vector<Message> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<Message> out;
  for (auto& m : p.Messages()) {
    if (m.userId != user) {
      continue;
    }
    if (auto typed = dynamic_cast<Message*>(m.message.get())) {
      out.push_back(*typed);
    }
  }
  return out;
}

// the relays of a sample sent to `user`: the raw bytes the sender's game
// sent ('{}' here, the speech case's shape), each one's reliability as 0 or 1
std::vector<int> RelaysTo(PartOne& p, Networking::UserId user)
{
  std::vector<int> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() && m.j.empty()) {
      out.push_back(m.reliable ? 1 : 0);
    }
  }
  return out;
}

struct World
{
  PartOne& p;
  MpObjectReference& prop;
  uint32_t cell;
};

// two players beside the prop in its cell, c1 hosting the cell
World Enter(PartOne& p)
{
  Player(p, 0, kC1, kHere + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 1, kC2, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  auto& prop = Prop(p, kBasket, kHere);
  const auto cell = p.worldState.CellOf(prop);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(prop.GetActorListeners().size() >= 2);
  p.Messages().clear();
  return World{ p, prop, *cell };
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}
}

TEST_CASE("A claim by a player in the prop's cell is granted and the owner "
          "word reaches every listener; a second player's claim takes the "
          "prop at the next epoch; the same player's again changes nothing",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  // the host simulates an untouched prop
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 0);

  // c2 touches it: c2 simulates, both seats hear the word
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  const auto toC1 = SentTo<PropOwnerMessage>(p, 0);
  const auto toC2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(toC1.size() == 1);
  REQUIRE(toC2.size() == 1);
  REQUIRE(toC2[0].refr == kBasket);
  REQUIRE(toC2[0].player == kC2);
  REQUIRE(toC2[0].epoch == 1);
  REQUIRE(toC1[0].player == kC2);

  // c1 kicks it back: the owner flips at epoch 2
  p.Messages().clear();
  Claim(p, 0, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  const auto back = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(back.size() == 1);
  REQUIRE(back[0].player == kC1);
  REQUIRE(back[0].epoch == 2);

  // the same player again: nothing goes out, the epoch stands
  p.Messages().clear();
  Claim(p, 0, kBasket, 0);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  Leave(p);
}

TEST_CASE("A claim from outside the prop's cell, on an unknown reference or "
          "on a grabbed prop is refused: no word, the owner stands",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);

  // c2 steps into the next square and touches from there
  c2.SetPos(kEast);
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 0);
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  c2.SetPos(kHere + NiPoint3{ 60.f, 0.f, 0.f });

  // a reference the server does not hold, and a player's actor
  Claim(p, 1, 0xff000dff, 0);
  Claim(p, 1, kC1, 0);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());

  // c1 grabs it; c2's contact cannot take it out of c1's hands, c1's drop
  // lets the next touch through
  Claim(p, 0, kBasket, 1);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  p.Messages().clear();
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  Claim(p, 0, kBasket, 2);
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  Leave(p);
}

TEST_CASE("Only the owner's transform moves the record and reaches the other "
          "listener, unreliable while awake and reliable at sleep; a "
          "non-owner's writes nothing; a departed toucher's prop is the "
          "host's again",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  p.Messages().clear();

  // the host is not the owner now: refused, the record untouched
  const NiPoint3 kicked = kHere + NiPoint3{ 100.f, 20.f, 0.f };
  Sample(p, 0, kBasket, kicked, false);
  REQUIRE(w.prop.GetPos() == kHere);
  REQUIRE(RelaysTo(p, 0).empty());
  REQUIRE(RelaysTo(p, 1).empty());

  // the owner's sample: the record moves, c1 alone hears it, unreliable
  Sample(p, 1, kBasket, kicked, false);
  REQUIRE(w.prop.GetPos() == kicked);
  REQUIRE(w.prop.GetAngle().z == 72.f);
  REQUIRE(RelaysTo(p, 1).empty());
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 0 });

  // the body rests: the final sample, reliable; the toucher keeps the prop
  // for the lease
  p.Messages().clear();
  const NiPoint3 rested = kicked + NiPoint3{ 20.f, 0.f, -10.f };
  Sample(p, 1, kBasket, rested, true);
  REQUIRE(w.prop.GetPos() == rested);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 1 });
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);

  // the toucher leaves: the host simulates it again, the epoch kept, and the
  // host's samples pass
  DoDisconnect(p, 1);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  p.Messages().clear();
  const NiPoint3 again = rested + NiPoint3{ 5.f, 0.f, 0.f };
  Sample(p, 0, kBasket, again, false);
  REQUIRE(w.prop.GetPos() == again);
  REQUIRE(RelaysTo(p, 0).empty());

  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

TEST_CASE("A claim on a container, a claim from beyond reach and a sample "
          "farther than the step budget are refused, the record standing; a "
          "listener arriving while a seat simulates the prop hears the owner",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);

  // a container is no prop: no word, no move
  const NiPoint3 chestAt = kHere + NiPoint3{ 30.f, 0.f, 0.f };
  {
    auto chest = std::make_unique<MpObjectReference>(
      LocationalData{ chestAt, { 0.f, 0.f, 0.f }, FormDesc::Tamriel() },
      p.CreateFormCallbacks(), kBarrelFood01NoRespawn, "CONT");
    p.worldState.AddForm(std::move(chest), kChest);
    p.worldState.GetFormAt<MpObjectReference>(kChest)
      .ForceSubscriptionsUpdate();
  }
  p.Messages().clear();
  Claim(p, 1, kChest, 0);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  Sample(p, 0, kChest, chestAt + NiPoint3{ 0.f, 0.f, -50.f }, false);
  REQUIRE(p.worldState.GetFormAt<MpObjectReference>(kChest).GetPos() ==
          chestAt);

  // beyond reach inside the square: refused, the host keeps the prop
  c2.SetPos(kHere + NiPoint3{ 400.f, 0.f, 0.f });
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  c2.SetPos(kHere + NiPoint3{ 60.f, 0.f, 0.f });

  // the step budget: the owner's sample from across the cell leaves the
  // record where it stands and reaches nobody; a near sample passes
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  p.Messages().clear();
  Sample(p, 1, kBasket, kHere + NiPoint3{ 5000.f, 0.f, 0.f }, false);
  REQUIRE(w.prop.GetPos() == kHere);
  REQUIRE(RelaysTo(p, 0).empty());
  const NiPoint3 near = kHere + NiPoint3{ 50.f, 0.f, 0.f };
  Sample(p, 1, kBasket, near, false);
  REQUIRE(w.prop.GetPos() == near);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 0 });

  // a third seat enters the cell while c2 simulates: its create word is
  // followed by the owner word at the current epoch
  p.Messages().clear();
  Player(p, 2, kC3, kHere + NiPoint3{ 80.f, 0.f, 0.f });
  const auto toC3 = SentTo<PropOwnerMessage>(p, 2);
  REQUIRE(toC3.size() == 1);
  REQUIRE(toC3[0].refr == kBasket);
  REQUIRE(toC3[0].player == kC2);
  REQUIRE(toC3[0].epoch == p.PropEpochOf(w.prop));

  DoDisconnect(p, 2);
  p.DestroyActor(kC3);
  Leave(p);
}
