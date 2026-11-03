#include "ActionListener.h"
#include "PropClaimMessage.h"
#include "PropOwnerMessage.h"
#include "PropTransformMessage.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <thread>

PartOne& GetPartOne();

// thuum docs/verbs/prop-physics.md (ADR-058): one simulating seat per physics
// prop. The cell's host simulates a prop by default; the seat whose player
// touched it takes it (the claim, R0, newest touch wins at the next epoch)
// and every listener of the prop hears the word (PropOwner); only the
// owner's transforms move the record (R2) and reach the prop's other
// listeners, unreliable while the body is awake and reliable at its sleep;
// a toucher's rest or departure hands its props back to the host with the
// word (player 0). The arbitration's clocks (the moving window, the grab's
// cap) are the Rust rule's tests (wire-rules props.rs): the clock cannot be
// advanced from here.

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kC1 = 0xff000d01;
constexpr uint32_t kC2 = 0xff000d02;
constexpr uint32_t kBasket = 0xff000d03;
constexpr uint32_t kC3 = 0xff000d04;
constexpr uint32_t kChest = 0xff000d05;
constexpr uint32_t kC4 = 0xff000d08;
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

void CellLoaded(PartOne& p, Networking::UserId user, uint32_t cell,
                bool loaded = true)
{
  DoMessage(p, user,
            nlohmann::json{ { "t", MsgType::CellLoaded },
                            { "cell", cell },
                            { "loaded", loaded } });
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
// `user`'s movement report for its own actor (the square change refreshes
// its reach, PartOne::RefreshReach)
void Move(PartOne& p, Networking::UserId user, uint32_t actorId,
          const NiPoint3& pos)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = p.worldState.GetFormAt<MpActor>(actorId).GetIdx();
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { 0, 0, 0 };
  msg.data.worldOrCell = 0x3c;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

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

// thuum docs/verbs/prop-physics.md (ADR-058 slice two): a movable static the
// settings list (`propStatics`) is a prop; any other MSTT is not. The bases:
// HandCart01Wheel, Skyrim.esm's MSTT 0x1c0c3 (lab/esm.py 2026-11-02: the
// Riverwood smithy's cart wheel, placed as 0x1c0c2 and 0x14201), and the
// Raise Wall's BFXSmashedBarrelWithOil 0xeee0d (SpellPlacedTest's MSTT)
namespace {
constexpr uint32_t kHandCartWheel = 0x1c0c3;
constexpr uint32_t kWallBase = 0xeee0d;
constexpr uint32_t kWheel = 0xff000d06;
constexpr uint32_t kWall = 0xff000d07;

MpObjectReference& Static(PartOne& p, uint32_t id, uint32_t base,
                          const NiPoint3& pos)
{
  auto refr = std::make_unique<MpObjectReference>(
    LocationalData{ pos, { 0.f, 0.f, 0.f }, FormDesc::Tamriel() },
    p.CreateFormCallbacks(), base, "MSTT");
  p.worldState.AddForm(std::move(refr), id);
  auto& placed = p.worldState.GetFormAt<MpObjectReference>(id);
  placed.ForceSubscriptionsUpdate();
  return placed;
}
}

TEST_CASE("A movable static the settings list is a prop: a claim on it is "
          "granted and its owner's sample moves the record; an unlisted "
          "movable static (a Raise Wall's) is refused as no prop; a bad "
          "entry throws",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  p.SetPropStaticsSettings("[\"1c0c3:Skyrim.esm\"]");
  REQUIRE(p.worldState.IsPropStatic(kHandCartWheel));
  REQUIRE_FALSE(p.worldState.IsPropStatic(kWallBase));

  Player(p, 0, kC1, kHere + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 1, kC2, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  auto& wheel = Static(p, kWheel, kHandCartWheel, kHere);
  auto& wall = Static(p, kWall, kWallBase, kHere + NiPoint3{ 20.f, 0.f, 0.f });
  const auto cell = p.worldState.CellOf(wheel);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(p.PropOwnerOf(wheel) == kC1);
  p.Messages().clear();

  // c2 touches the wheel: granted, both seats hear the word
  Claim(p, 1, kWheel, 0);
  REQUIRE(p.PropOwnerOf(wheel) == kC2);
  REQUIRE(p.PropEpochOf(wheel) == 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).size() == 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).size() == 1);
  // its sample moves the record and reaches the other seat
  p.Messages().clear();
  const NiPoint3 rolled = kHere + NiPoint3{ 50.f, 0.f, 0.f };
  Sample(p, 1, kWheel, rolled, false);
  REQUIRE(wheel.GetPos() == rolled);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 0 });

  // the wall is no prop: no word, no move
  p.Messages().clear();
  Claim(p, 1, kWall, 0);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  Sample(p, 0, kWall, kHere + NiPoint3{ 20.f, 0.f, -50.f }, false);
  REQUIRE(wall.GetPos() == kHere + NiPoint3{ 20.f, 0.f, 0.f });

  // the list cleared, the wheel is no prop either; a bad entry throws
  p.SetPropStaticsSettings("[]");
  REQUIRE_FALSE(p.worldState.IsPropStatic(kHandCartWheel));
  REQUIRE_THROWS(p.SetPropStaticsSettings("[\"zz:Skyrim.esm\"]"));
  REQUIRE_THROWS(p.SetPropStaticsSettings("[\"0:Skyrim.esm\"]"));
  REQUIRE_THROWS(p.SetPropStaticsSettings("{}"));
  Leave(p);
}

TEST_CASE("A claim by a player in the prop's cell is granted and the owner "
          "word reaches every listener; a second player's contact is refused "
          "while the first streams it and takes the prop at the next epoch "
          "once it rests; the same player's again changes nothing",
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

  // c1 walks into it while c2 streams it awake: refused as moving (ADR-063
  // decision 4, playtest 41), the owner and the epoch stand, and the
  // claimant alone hears the current word
  p.Messages().clear();
  Claim(p, 0, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  const auto answered = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(answered.size() == 1);
  REQUIRE(answered[0].player == kC2);
  REQUIRE(answered[0].epoch == 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());

  // c2's body rests: the host's again at once, at epoch 2, both seats told
  // with player 0 (the host by default); the answer above cleared first
  p.Messages().clear();
  Sample(p, 1, kBasket, kHere + NiPoint3{ 30.f, 0.f, 0.f }, true);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  const auto rest1 = SentTo<PropOwnerMessage>(p, 0);
  const auto rest2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(rest1.size() == 1);
  REQUIRE(rest2.size() == 1);
  REQUIRE(rest2[0].player == 0);
  REQUIRE(rest2[0].epoch == 2);

  // c1 kicks it at rest: the owner flips at epoch 3
  p.Messages().clear();
  Claim(p, 0, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  const auto back = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(back.size() == 1);
  REQUIRE(back[0].player == kC1);
  REQUIRE(back[0].epoch == 3);

  // the same player again: nothing goes out, the epoch stands
  p.Messages().clear();
  Claim(p, 0, kBasket, 0);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  Leave(p);
}

TEST_CASE("A claim on an unknown reference or on a grabbed prop is refused: "
          "the claimant alone hears the current word, the owner stands; a "
          "claim from outside the prop's cell is granted (ADR-063 decision 4)",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);

  // c2 steps into the next square and touches from there: granted all the
  // same (thuum docs/DECISIONS.md ADR-063 decision 4, playtest 41's carried
  // bucket: the claimant's cell and its distance to a record it may already
  // have moved are no gate; ownership is arbitrated, not permitted)
  c2.SetPos(kEast);
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  REQUIRE_FALSE(SentTo<PropOwnerMessage>(p, 0).empty());
  REQUIRE_FALSE(SentTo<PropOwnerMessage>(p, 1).empty());
  c2.SetPos(kHere + NiPoint3{ 60.f, 0.f, 0.f });
  p.Messages().clear();

  // a reference the server does not hold, and a player's actor
  Claim(p, 1, 0xff000dff, 0);
  Claim(p, 1, kC1, 0);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());

  // c1 grabs it while c2 streams it (a grab beats a touch, epoch 2); c2's
  // contact cannot take it out of c1's hands, and c2 hears the word
  Claim(p, 0, kBasket, 1);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  p.Messages().clear();
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  const auto held = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(held.size() == 1);
  REQUIRE(held[0].player == kC1);
  REQUIRE(held[0].epoch == 2);
  // c1's drop: the fall is c1's (c2's contact on it is moving, refused); its
  // rest returns it to the host by a word (c1 hosts: the word says player 0
  // all the same, epoch 3), then c2's contact takes it at 4
  Claim(p, 0, kBasket, 2);
  p.Messages().clear();
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  p.Messages().clear();
  Sample(p, 0, kBasket, kHere + NiPoint3{ 10.f, 0.f, 0.f }, true);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  const auto rested = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(rested.size() == 1);
  REQUIRE(rested[0].player == 0);
  REQUIRE(rested[0].epoch == 3);
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 4);
  Leave(p);
}

TEST_CASE("Only the owner's transform moves the record and reaches the other "
          "listener, unreliable while awake and reliable at sleep; a "
          "non-owner's writes nothing and is answered with the word; the "
          "owner's rest returns the prop to the host with the word, and so "
          "does its departure",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  p.Messages().clear();

  // the host is not the owner now: refused, the record untouched, and the
  // sender hears the current word (ADR-063 decision 4: a divergent mirror
  // heals instead of streaming into refusals)
  const NiPoint3 kicked = kHere + NiPoint3{ 100.f, 20.f, 0.f };
  Sample(p, 0, kBasket, kicked, false);
  REQUIRE(w.prop.GetPos() == kHere);
  REQUIRE(RelaysTo(p, 0).empty());
  REQUIRE(RelaysTo(p, 1).empty());
  const auto answer = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(answer.size() == 1);
  REQUIRE(answer[0].player == kC2);
  REQUIRE(answer[0].epoch == 1);

  // the owner's sample: the record moves, c1 alone hears it, unreliable
  Sample(p, 1, kBasket, kicked, false);
  REQUIRE(w.prop.GetPos() == kicked);
  REQUIRE(w.prop.GetAngle().z == 72.f);
  REQUIRE(RelaysTo(p, 1).empty());
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 0 });

  // the body rests: the final sample, reliable, and the prop is the host's
  // again at once, both seats told with player 0 at epoch 2 (ADR-063
  // decision 4: the return is a word, by the owner's own final sample)
  p.Messages().clear();
  const NiPoint3 rested = kicked + NiPoint3{ 20.f, 0.f, -10.f };
  Sample(p, 1, kBasket, rested, true);
  REQUIRE(w.prop.GetPos() == rested);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 1 });
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  const auto rest1 = SentTo<PropOwnerMessage>(p, 0);
  const auto rest2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(rest1.size() == 1);
  REQUIRE(rest2.size() == 1);
  REQUIRE(rest2[0].player == 0);
  REQUIRE(rest2[0].epoch == 2);

  // the toucher takes it again and leaves mid-flight: the departure returns
  // it to the host with the word (epoch 4), and the host's samples pass
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  p.Messages().clear();
  DoDisconnect(p, 1);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 4);
  const auto left = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(left.size() == 1);
  REQUIRE(left[0].player == 0);
  REQUIRE(left[0].epoch == 4);
  p.Messages().clear();
  const NiPoint3 again = rested + NiPoint3{ 5.f, 0.f, 0.f };
  Sample(p, 0, kBasket, again, false);
  REQUIRE(w.prop.GetPos() == again);
  REQUIRE(RelaysTo(p, 0).empty());

  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

TEST_CASE("A claim on a container is refused and one from beyond reach is "
          "granted (ADR-063); a sample farther than the step budget is "
          "refused, the record standing; a listener arriving while a seat "
          "simulates the prop hears the owner",
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

  // beyond arm's reach inside the square: granted all the same (ADR-063
  // decision 4: the distance to the record is no gate; the reach budgets of
  // 256, 512 and 2048 units are gone with it)
  c2.SetPos(kHere + NiPoint3{ 400.f, 0.f, 0.f });
  p.Messages().clear();
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE_FALSE(SentTo<PropOwnerMessage>(p, 1).empty());
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
  // followed by the owner word at the current epoch; a fourth, after c2's
  // rest returned the prop, hears the host's word (player 0) at the epoch
  // the return gave it, so no late seat keeps an older owner
  p.Messages().clear();
  Player(p, 2, kC3, kHere + NiPoint3{ 80.f, 0.f, 0.f });
  const auto toC3 = SentTo<PropOwnerMessage>(p, 2);
  REQUIRE(toC3.size() == 1);
  REQUIRE(toC3[0].refr == kBasket);
  REQUIRE(toC3[0].player == kC2);
  REQUIRE(toC3[0].epoch == p.PropEpochOf(w.prop));
  {
    Sample(p, 1, kBasket, near + NiPoint3{ 5.f, 0.f, 0.f }, true);
    const auto epochAtRest = p.PropEpochOf(w.prop);
    REQUIRE(epochAtRest == toC3[0].epoch + 1);
    p.Messages().clear();
    Player(p, 3, kC4, kHere + NiPoint3{ 90.f, 0.f, 0.f });
    const auto toC4 = SentTo<PropOwnerMessage>(p, 3);
    REQUIRE(toC4.size() == 1);
    REQUIRE(toC4[0].player == 0);
    REQUIRE(toC4[0].epoch == epochAtRest);
    DoDisconnect(p, 3);
    p.DestroyActor(kC4);
  }

  DoDisconnect(p, 2);
  p.DestroyActor(kC3);
  Leave(p);
}

TEST_CASE("The host's own sample of a resting prop wakes it: the row the "
          "host's at a new epoch and the word names the host by id, so a "
          "host change leaves it streaming until its rest (ADR-063 decision "
          "4, the review of 2026-11-03)",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  // the host's engine kicks the untouched basket: its first awake sample
  // wakes it, both seats hear the host by name at epoch 1
  p.Messages().clear();
  Sample(p, 0, kBasket, kHere + NiPoint3{ 20.f, 0.f, 0.f }, false);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropLiveOwner(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  REQUIRE(std::abs(w.prop.GetPos().x - (kHere.x + 20.f)) < 1.f);
  const auto woke1 = SentTo<PropOwnerMessage>(p, 0);
  const auto woke2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(woke1.size() == 1);
  REQUIRE(woke2.size() == 1);
  REQUIRE(woke2[0].player == kC1);
  REQUIRE(woke2[0].epoch == 1);
  // the next awake sample is the live row's: no word
  p.Messages().clear();
  Sample(p, 0, kBasket, kHere + NiPoint3{ 25.f, 0.f, 0.f }, false);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());

  // the props table's half of a grant to c2 mid-flight (the cells table's
  // is the rules test's): c1's live row survives, c2's sample is refused
  // and answered with c1's name, c1's samples still pass, and c1's rest
  // returns the prop to the host by default with the word (player 0)
  p.PropsHostChanged(w.cell, kC2);
  p.Messages().clear();
  Sample(p, 1, kBasket, kHere + NiPoint3{ 60.f, 0.f, 0.f }, false);
  REQUIRE(std::abs(w.prop.GetPos().x - (kHere.x + 25.f)) < 1.f);
  const auto answered = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(answered.size() == 1);
  REQUIRE(answered[0].player == kC1);
  REQUIRE(answered[0].epoch == 1);
  Sample(p, 0, kBasket, kHere + NiPoint3{ 30.f, 0.f, 0.f }, false);
  REQUIRE(std::abs(w.prop.GetPos().x - (kHere.x + 30.f)) < 1.f);
  p.Messages().clear();
  Sample(p, 0, kBasket, kHere + NiPoint3{ 32.f, 0.f, 0.f }, true);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  const auto rested = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(rested.size() == 1);
  REQUIRE(rested[0].player == 0);
  REQUIRE(rested[0].epoch == 2);
  Leave(p);
}

TEST_CASE("A player's unload of a cell returns its live props there to the "
          "host with a word each: the 3D went with the cell before any final "
          "sample (the review of 2026-11-03, D2)",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  // c2 kicks the basket and streams it awake, then its game unloads the
  // cell (a door): the row returns to the host at a new epoch, both seats
  // told with player 0; c2's late sample is refused NotOwner and answered
  // (a guest's; Away is a host's wake or a claim)
  Claim(p, 1, kBasket, 0);
  Sample(p, 1, kBasket, kHere + NiPoint3{ 20.f, 0.f, 0.f }, false);
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  p.Messages().clear();
  CellLoaded(p, 1, w.cell, false);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  const auto toC1 = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(toC1.size() == 1);
  REQUIRE(toC1[0].player == 0);
  REQUIRE(toC1[0].epoch == 2);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).size() == 1);
  p.Messages().clear();
  Sample(p, 1, kBasket, kHere + NiPoint3{ 40.f, 0.f, 0.f }, false);
  REQUIRE(std::abs(w.prop.GetPos().x - (kHere.x + 20.f)) < 1.f);
  // the unload is a state: c2's claim there is refused until its game
  // reports the cell loaded again or has the cell back in its reach (the
  // review of 2026-11-03)
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  // c2 steps into the next square (a report may move an actor 4096 at most):
  // its reach is refreshed, the cell is in it, the state ends, the claim is
  // granted (round seven's reach exit, the follows' shape)
  Move(p, 1, kC2, kHere + NiPoint3{ 3900.f, 0.f, 0.f });
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  Sample(p, 1, kBasket, kHere + NiPoint3{ 22.f, 0.f, 0.f }, true);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == 4);
  Move(p, 1, kC2, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  // the host's own kick woke it (a word by id); its unload returns it too
  p.Messages().clear();
  Sample(p, 0, kBasket, kHere + NiPoint3{ 25.f, 0.f, 0.f }, false);
  REQUIRE(p.PropLiveOwner(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 5);
  p.Messages().clear();
  CellLoaded(p, 0, w.cell, false);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == 6);
  const auto toC2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(toC2.size() == 1);
  REQUIRE(toC2[0].player == 0);
  REQUIRE(toC2[0].epoch == 6);
  // the host still hosts through its grace, but its late awake sample wakes
  // nothing until its game reports the cell loaded again; then it does
  Sample(p, 0, kBasket, kHere + NiPoint3{ 35.f, 0.f, 0.f }, false);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == 6);
  // the record stands where the wake's sample put it (pipeline 4848: this
  // line once expected a rest sample the case never sends)
  REQUIRE(std::abs(w.prop.GetPos().x - (kHere.x + 25.f)) < 1.f);
  CellLoaded(p, 0, w.cell, true);
  Sample(p, 0, kBasket, kHere + NiPoint3{ 36.f, 0.f, 0.f }, false);
  REQUIRE(p.PropLiveOwner(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 7);
  Leave(p);
}
