#include "ActionListener.h"
#include "ActivateMessage.h"
#include "PropClaimMessage.h"
#include "PropOwnerMessage.h"
#include "PropTransformMessage.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <cmath>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum docs/verbs/prop-physics.md (ADR-058; A1 to A7 of ADR-071 decision 2,
// physics rung 1): one simulating seat per physics prop. The cell's host
// simulates a prop by default; a toucher takes it at rest (the claim, R0, at
// the next epoch), a grab takes it while it moves and nothing takes it from
// a grab, and every listener of the prop hears the word (PropOwner); only
// the owner's samples move the record (R2) and reach the prop's other
// listeners, unreliable while the body is awake and reliable at its sleep;
// the owner's rest, its departure and its unload of the cell return the prop
// to the host with the word (player 0), the host's own wake names it by id,
// and a refused claim or sample is answered with the current word. No clock
// changes an owner; the moving window that judges a claim is the Rust rule's
// test (wire-rules props.rs): the clock cannot be advanced from here.

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kC1 = 0xff000d01;
constexpr uint32_t kC2 = 0xff000d02;
constexpr uint32_t kBasket = 0xff000d03;
constexpr uint32_t kC3 = 0xff000d04;
constexpr uint32_t kChest = 0xff000d05;
constexpr uint32_t kC4 = 0xff000d08;
constexpr uint32_t kC5 = 0xff000d09;
constexpr uint32_t kC6 = 0xff000d0a;
// BarrelFood01_NoRespawn, a CONT of Skyrim.esm (esm.py): no prop
constexpr uint32_t kBarrelFood01NoRespawn = 0x10e882;
// Bucket01, a MISC of Skyrim.esm (the one Riverwood's 0x2ad9c is placed from)
constexpr uint32_t kBucket01 = 0x12fdf;
// the exterior grid square (2, 0) of Tamriel (CellHostTest's kHere), the
// next square east, and two squares east (MpObjectReference's grid square of
// x 17500 is 4, the prop's 2: outside the 3x3 its subscriptions keep)
const NiPoint3 kHere{ 9000.f, 500.f, 0.f };
const NiPoint3 kEast{ 13500.f, 500.f, 0.f };
const NiPoint3 kTwoEast{ 17500.f, 500.f, 0.f };

MpActor& Player(PartOne& p, Networking::UserId user, uint32_t id,
                const NiPoint3& pos, uint32_t worldOrCell = kTamriel)
{
  DoConnect(p, user);
  p.CreateActor(id, pos, 0, worldOrCell);
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

// whether `actor` is one of `prop`'s listeners: its game was sent the prop's
// create word and not told it is gone (the 3x3 grid squares its
// subscriptions keep)
bool Listens(const MpObjectReference& prop, const MpActor& actor)
{
  const auto& listeners = prop.GetActorListeners();
  return std::find(listeners.begin(), listeners.end(), &actor) !=
    listeners.end();
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

// `user`'s activation of the reference `refr` by its own player: an item's
// is its pickup (MpObjectReference::ProcessActivateNormal's pickable branch)
void PickUp(PartOne& p, Networking::UserId user, uint32_t refr)
{
  ActivateMessage msg;
  msg.data.caster = 0x14;
  msg.data.target = refr;
  msg.data.isSecondActivation = false;
  p.GetActionListener().OnActivate(Raw(user), msg);
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

// thuum D-S3 (traps follow the interaction, point 4): Embershard Mine
// 0xb6be6's falling rock 0xb6d45 (TrapFallingRockSm01 0x79922, an ACTI) at
// (2915.7, 3613.4, 8840.6) and its sibling 0xb6d48 at (2805.9, 3627.0,
// 8840.6), enabled records whose XAPR names the linker 0xb6d3e (lab/esm.py
// ref on Skyrim.esm, 2026-11-05); the players stand about 100 units from
// both
constexpr uint32_t kEmbershard = 0xb6be6;
constexpr uint32_t kRock = 0xb6d45;
constexpr uint32_t kSiblingRock = 0xb6d48;
const NiPoint3 kByTheRocks{ 2860.f, 3520.f, 8840.f };

// the default logger's lines at info and above while the guard lives
// (TrapRelayTest's shape), restored at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("prop-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~LogCapture() { spdlog::set_default_logger(old); }
  size_t Count(const std::string& line) const
  {
    const std::string log = oss.str();
    size_t n = 0;
    for (size_t at = log.find(line); at != std::string::npos;
         at = log.find(line, at + line.size())) {
      ++n;
    }
    return n;
  }
};
}

TEST_CASE("A contact at rest is granted and the owner word reaches every "
          "listener; a second seat's contact on the moving prop is refused "
          "E_PROP_MOVING and answered; at rest the next touch takes it at "
          "the next epoch; the same player's again changes nothing",
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

  // c1 walks into it while c2 streams it awake (the grant counts as a
  // sample): refused as moving (A3), the owner and the epoch stand, and the
  // claimant alone hears the current word (A5)
  p.Messages().clear();
  {
    LogCapture log;
    Claim(p, 0, kBasket, 0);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_MOVING: ff000d01 claims "
                      "prop ff000d03 by touch 0; another seat streams it "
                      "awake: a contact, a drop or an impulse takes a prop at "
                      "rest, a grab takes it at once") == 1);
  }
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  const auto answered = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(answered.size() == 1);
  REQUIRE(answered[0].player == kC2);
  REQUIRE(answered[0].epoch == 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());

  // c2's body rests: the host's again at once, at epoch 2, both seats told
  // with player 0, the host by default (A4)
  p.Messages().clear();
  Sample(p, 1, kBasket, kHere + NiPoint3{ 30.f, 0.f, 0.f }, true);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  const auto rest1 = SentTo<PropOwnerMessage>(p, 0);
  const auto rest2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(rest1.size() == 1);
  REQUIRE(rest2.size() == 1);
  REQUIRE(rest1[0].player == 0);
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

TEST_CASE("A claim from the next square is granted and one from two squares "
          "away is refused E_PROP_UNSEEN (the listener check in place of the "
          "cell and reach gates); an unknown reference and a grabbed prop "
          "are refused, the grab's loser hearing who holds it",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);

  // c2 steps into the next square and touches from there: granted, where
  // the claimant-in-cell gate refused it E_PROP_CELL (playtest 44's kettle
  // at the mill's cell edge); it is still a listener of the prop
  c2.SetPos(kEast);
  REQUIRE(Listens(w.prop, c2));
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  REQUIRE_FALSE(SentTo<PropOwnerMessage>(p, 0).empty());
  REQUIRE_FALSE(SentTo<PropOwnerMessage>(p, 1).empty());
  // its rest returns the prop to the host (epoch 2)
  Sample(p, 1, kBasket, kHere + NiPoint3{ 10.f, 0.f, 0.f }, true);
  REQUIRE(p.PropEpochOf(w.prop) == 2);

  // two squares away c2 is no listener of the prop, so its claim is refused
  // with no word (its game has no copy to heal) and nothing changes
  c2.SetPos(kTwoEast);
  REQUIRE_FALSE(Listens(w.prop, c2));
  p.Messages().clear();
  {
    LogCapture log;
    Claim(p, 1, kBasket, 0);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_UNSEEN: ff000d02 claims "
                      "prop ff000d03 by touch 0; the claimant is no listener "
                      "of the prop (its game was never sent it, or was told "
                      "it is gone)") == 1);
  }
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  c2.SetPos(kHere + NiPoint3{ 60.f, 0.f, 0.f });
  REQUIRE(Listens(w.prop, c2));
  p.Messages().clear();

  // a reference the server does not hold, and a player's actor
  Claim(p, 1, 0xff000dff, 0);
  Claim(p, 1, kC1, 0);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());

  // c1 grabs it at rest (epoch 3); c2's contact cannot take it out of c1's
  // hands, and c2 hears who holds it
  Claim(p, 0, kBasket, 1);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  p.Messages().clear();
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  const auto held = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(held.size() == 1);
  REQUIRE(held[0].player == kC1);
  REQUIRE(held[0].epoch == 3);
  // c1's release: the fall is c1's (c2's contact on it is moving, refused);
  // its rest returns it to the host by a word (c1 hosts: the word says
  // player 0 all the same, epoch 4), then c2's contact takes it at 5
  Claim(p, 0, kBasket, 2);
  p.Messages().clear();
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  p.Messages().clear();
  Sample(p, 0, kBasket, kHere + NiPoint3{ 10.f, 0.f, 0.f }, true);
  REQUIRE(p.PropEpochOf(w.prop) == 4);
  const auto rested = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(rested.size() == 1);
  REQUIRE(rested[0].player == 0);
  REQUIRE(rested[0].epoch == 4);
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 5);
  Leave(p);
}

TEST_CASE("A player holds one prop at a time: its grab of a second prop is "
          "granted and ends its hold of the first, which stays its awake "
          "row, so the host's contact on it is refused E_PROP_MOVING and "
          "answered",
          "[Prop][espm]")
{
  // the engine holds one grabbed object per player (CommonLibSSE-NG
  // P/PlayerCharacter.h:697): a new grab means the old hold is over, whether
  // or not its release reached the server
  constexpr uint32_t kTub = 0xff000d0b;
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& tub = Prop(p, kTub, kHere + NiPoint3{ 20.f, 0.f, 0.f });
  REQUIRE(Listens(tub, p.worldState.GetFormAt<MpActor>(kC2)));
  Claim(p, 1, kBasket, 1);
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  p.Messages().clear();

  // the second grab: granted, both seats told
  Claim(p, 1, kTub, 1);
  REQUIRE(p.PropLiveOwner(tub) == kC2);
  REQUIRE(p.PropEpochOf(tub) == 1);
  const auto toC1 = SentTo<PropOwnerMessage>(p, 0);
  const auto toC2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(toC1.size() == 1);
  REQUIRE(toC2.size() == 1);
  REQUIRE(toC1[0].refr == kTub);
  REQUIRE(toC1[0].player == kC2);
  REQUIRE(toC2[0].player == kC2);
  REQUIRE(toC2[0].epoch == 1);

  // the basket stays c2's awake row, held no more: the host's contact on it
  // is refused as moving (inside the window from c2's grant), not as held,
  // and the host hears c2's word
  p.Messages().clear();
  {
    LogCapture log;
    Claim(p, 0, kBasket, 0);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_MOVING: ff000d01 claims "
                      "prop ff000d03 by touch 0; another seat streams it "
                      "awake: a contact, a drop or an impulse takes a prop at "
                      "rest, a grab takes it at once") == 1);
    REQUIRE(log.Count("E_PROP_HELD") == 0);
  }
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  const auto answered = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(answered.size() == 1);
  REQUIRE(answered[0].refr == kBasket);
  REQUIRE(answered[0].player == kC2);
  REQUIRE(answered[0].epoch == 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  Leave(p);
}

TEST_CASE("A pickup is a take: a player's pickup of a prop another player "
          "holds is refused E_PROP_HELD and of one another seat streams awake "
          "E_PROP_MOVING, each answered with the word, the prop standing for "
          "both seats; the owner's own pickup and a resting prop's are taken "
          "as before",
          "[Prop][espm]")
{
  // a grab the engine refuses in its own frame ends in a pickup at the key's
  // release (havok-props.md E16 and E28), and the pickup deleted a prop
  // another seat was moving, for every seat (x-moving-press 20261112-082448)
  constexpr uint32_t kTub = 0xff000d0b;
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);
  auto& tub = Prop(p, kTub, kHere + NiPoint3{ 20.f, 0.f, 0.f });
  REQUIRE(Listens(tub, c1));
  REQUIRE(c1.GetInventory().GetItemCount(kBucket01) == 0);
  REQUIRE(c2.GetInventory().GetItemCount(kBucket01) == 0);
  // c2 holds the basket and streams the tub awake (its kick)
  Claim(p, 1, kBasket, 1);
  Claim(p, 1, kTub, 0);
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  REQUIRE(p.PropLiveOwner(tub) == kC2);
  p.Messages().clear();

  // c1 picks up each: refused, logged and answered with c2's word, and
  // nothing else happens for either seat
  {
    LogCapture log;
    PickUp(p, 0, kBasket);
    PickUp(p, 0, kTub);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_HELD: ff000d01 picks up "
                      "prop ff000d03; another player holds it grabbed") == 1);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_MOVING: ff000d01 picks up "
                      "prop ff000d0b; another seat streams it awake: a pickup "
                      "takes a prop at rest, a grab takes it at once") == 1);
    REQUIRE(log.Count("Deleting 0xff item") == 0);
  }
  for (auto* prop : { &w.prop, &tub }) {
    REQUIRE_FALSE(prop->IsDeleted());
    REQUIRE_FALSE(prop->IsHarvested());
    REQUIRE(Listens(*prop, c1));
    REQUIRE(Listens(*prop, c2));
    REQUIRE(p.PropLiveOwner(*prop) == kC2);
    REQUIRE(p.PropEpochOf(*prop) == 1);
  }
  REQUIRE(c1.GetInventory().GetItemCount(kBucket01) == 0);
  const auto answered = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(answered.size() == 2);
  REQUIRE(answered[0].refr == kBasket);
  REQUIRE(answered[1].refr == kTub);
  for (const auto& word : answered) {
    REQUIRE(word.player == kC2);
    REQUIRE(word.epoch == 1);
  }
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());

  // c2's pickup of the tub it streams is its own row's, taken as before: the
  // item in its inventory, the server-made reference deleted for both seats
  PickUp(p, 1, kTub);
  REQUIRE(tub.IsDeleted());
  REQUIRE_FALSE(Listens(tub, c1));
  REQUIRE_FALSE(Listens(tub, c2));
  REQUIRE(c2.GetInventory().GetItemCount(kBucket01) == 1);

  // c2 lets the basket go and it rests, the host's again: c1's pickup of the
  // resting prop is taken
  Claim(p, 1, kBasket, 2);
  Sample(p, 1, kBasket, kHere + NiPoint3{ 10.f, 0.f, 0.f }, true);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  PickUp(p, 0, kBasket);
  REQUIRE(w.prop.IsDeleted());
  REQUIRE(c1.GetInventory().GetItemCount(kBucket01) == 1);
  Leave(p);
}

TEST_CASE("Only the owner's transform moves the record and reaches the other "
          "listener, unreliable while awake and reliable at sleep; a "
          "non-owner's writes nothing and is answered with the word; the "
          "owner's rest returns the prop to the host with the word at the "
          "next epoch, and so does a live owner's departure",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  p.Messages().clear();

  // the host is not the owner now: refused, the record untouched, and the
  // sender hears the current word (A5: a divergent word heals instead of
  // streaming into refusals)
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
  // again at once, both seats told with player 0 at epoch 2 (A4: the return
  // is a word, by the owner's own final sample)
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
  // it to the host with the word (epoch 4), and the host's own sample wakes
  // it (a word by id, epoch 5)
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
  REQUIRE(p.PropLiveOwner(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 5);

  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

TEST_CASE("A claim on a container is refused and one beyond the old reach "
          "inside the square is granted; a sample farther than the step "
          "budget is refused, the record standing, and an asleep one rests "
          "the prop where the record stands with the word; a late listener "
          "hears the current word, player 0 for a resting prop",
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

  // beyond the old contact reach (256 units) inside the square: granted all
  // the same (ADR-063 decision 4: the distance to the record is no gate;
  // the reach budgets of 256, 512 and 2048 units are gone)
  c2.SetPos(kHere + NiPoint3{ 400.f, 0.f, 0.f });
  p.Messages().clear();
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC2);
  REQUIRE_FALSE(SentTo<PropOwnerMessage>(p, 1).empty());
  c2.SetPos(kHere + NiPoint3{ 60.f, 0.f, 0.f });

  // the step budget: the owner's sample from across the cell leaves the
  // record where it stands and reaches nobody; a near sample passes
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

  // an asleep sample over the budget: the position is refused (the record
  // stands, nothing relayed), the rest is taken: the host's again at the
  // next epoch, every listener told with player 0 (RestRefused)
  const auto epochAtFlight = p.PropEpochOf(w.prop);
  p.Messages().clear();
  Sample(p, 1, kBasket, kHere + NiPoint3{ 5000.f, 0.f, 0.f }, true);
  REQUIRE(w.prop.GetPos() == near);
  REQUIRE(RelaysTo(p, 0).empty());
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == epochAtFlight + 1);
  const auto refusedRest = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(refusedRest.size() == 1);
  REQUIRE(refusedRest[0].player == 0);
  REQUIRE(refusedRest[0].epoch == epochAtFlight + 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 2).size() == 1);

  // a fourth seat, after the rest, hears the host's word (player 0) at the
  // epoch the rest gave it, so no late seat keeps an older owner
  p.Messages().clear();
  Player(p, 3, kC4, kHere + NiPoint3{ 90.f, 0.f, 0.f });
  const auto toC4 = SentTo<PropOwnerMessage>(p, 3);
  REQUIRE(toC4.size() == 1);
  REQUIRE(toC4[0].player == 0);
  REQUIRE(toC4[0].epoch == epochAtFlight + 1);

  DoDisconnect(p, 3);
  p.DestroyActor(kC4);
  DoDisconnect(p, 2);
  p.DestroyActor(kC3);
  Leave(p);
}

TEST_CASE("The host's own sample of a resting prop wakes it: the row the "
          "host's at a new epoch and the word names the host by id; a grant "
          "of the cell to the other seat changes no row, so the old host "
          "streams on until its rest, whose word names nobody (A7)",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  // c1 hosts the prop's square from the next square east (its load
  // report), a ring away, so a seat in the square itself takes the cell
  // from it (CellHostTest's host swap)
  auto& c1 = Player(p, 0, kC1, kEast);
  auto& prop = Prop(p, kBasket, kHere);
  const auto cell = p.worldState.CellOf(prop);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(Listens(prop, c1));
  p.Messages().clear();

  // the host's engine kicks the untouched basket: its first awake sample
  // wakes it, the host hearing itself by name at epoch 1; its next awake
  // sample is the live row's, no word
  Sample(p, 0, kBasket, kHere + NiPoint3{ 20.f, 0.f, 0.f }, false);
  REQUIRE(p.PropOwnerOf(prop) == kC1);
  REQUIRE(p.PropLiveOwner(prop) == kC1);
  REQUIRE(p.PropEpochOf(prop) == 1);
  REQUIRE(std::abs(prop.GetPos().x - (kHere.x + 20.f)) < 1.f);
  const auto woke = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(woke.size() == 1);
  REQUIRE(woke[0].player == kC1);
  REQUIRE(woke[0].epoch == 1);
  p.Messages().clear();
  Sample(p, 0, kBasket, kHere + NiPoint3{ 25.f, 0.f, 0.f }, false);
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).empty());

  // c2 arrives in the prop's own square: its create word brings the live
  // word, c1 by name; its load report takes the cell from the ring away
  p.Messages().clear();
  Player(p, 1, kC2, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  const auto late = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(late.size() == 1);
  REQUIRE(late[0].player == kC1);
  REQUIRE(late[0].epoch == 1);
  CellLoaded(p, 1, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC2);

  // the grant changes no row: c1's live row survives, c2's sample is
  // refused and answered with c1's name, c1's samples still pass
  REQUIRE(p.PropLiveOwner(prop) == kC1);
  REQUIRE(p.PropOwnerOf(prop) == kC1);
  REQUIRE(p.PropEpochOf(prop) == 1);
  p.Messages().clear();
  Sample(p, 1, kBasket, kHere + NiPoint3{ 60.f, 0.f, 0.f }, false);
  REQUIRE(std::abs(prop.GetPos().x - (kHere.x + 25.f)) < 1.f);
  const auto answered = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(answered.size() == 1);
  REQUIRE(answered[0].player == kC1);
  REQUIRE(answered[0].epoch == 1);
  Sample(p, 0, kBasket, kHere + NiPoint3{ 30.f, 0.f, 0.f }, false);
  REQUIRE(std::abs(prop.GetPos().x - (kHere.x + 30.f)) < 1.f);

  // c1's rest returns the prop to the host by default, c2 now, with the
  // word (player 0) to both seats
  p.Messages().clear();
  Sample(p, 0, kBasket, kHere + NiPoint3{ 32.f, 0.f, 0.f }, true);
  REQUIRE(p.PropEpochOf(prop) == 2);
  REQUIRE(p.PropLiveOwner(prop) == 0);
  REQUIRE(p.PropOwnerOf(prop) == kC2);
  const auto rested = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(rested.size() == 1);
  REQUIRE(rested[0].player == 0);
  REQUIRE(rested[0].epoch == 2);
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).size() == 1);
  Leave(p);
}

TEST_CASE("A player's unload of a cell returns its live props there to the "
          "host with a word each, and its claims and wakes there are refused "
          "E_PROP_AWAY until its load report or the cell's return to its "
          "reach (D-PH2 A, the review of 2026-11-03)",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);
  // c2 kicks the basket and streams it awake, then its game unloads the
  // cell (a door): the row returns to the host at a new epoch, both seats
  // told with player 0; c2's late sample is refused NotOwner (a guest's;
  // Away is a host's wake or a claim)
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
  // reports the cell loaded again or has the cell back in its reach
  {
    LogCapture log;
    Claim(p, 1, kBasket, 0);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_AWAY: ff000d02 claims prop "
                      "ff000d03 by touch 0; its game unloaded the prop's cell "
                      "and has not loaded or reached it since") == 1);
  }
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  // c2 stands in the next square (the server's own placement, as a teleport
  // is) and its game reports THAT cell loaded, never the old one: the load
  // report refreshes its reach, the old cell is in it, the state ends and
  // the claim is granted (round seven's reach exit, the follows' shape)
  const NiPoint3 nextSquare = kHere + NiPoint3{ 3900.f, 0.f, 0.f };
  c2.SetPos(nextSquare);
  const auto nextCell = p.worldState.CellOf(kTamriel, nextSquare);
  REQUIRE(nextCell.has_value());
  REQUIRE(*nextCell != w.cell);
  CellLoaded(p, 1, *nextCell, true);
  Claim(p, 1, kBasket, 0);
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  Sample(p, 1, kBasket, kHere + NiPoint3{ 22.f, 0.f, 0.f }, true);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == 4);
  c2.SetPos(kHere + NiPoint3{ 60.f, 0.f, 0.f });

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
  REQUIRE(std::abs(w.prop.GetPos().x - (kHere.x + 25.f)) < 1.f);
  CellLoaded(p, 0, w.cell, true);
  Sample(p, 0, kBasket, kHere + NiPoint3{ 36.f, 0.f, 0.f }, false);
  REQUIRE(p.PropLiveOwner(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 7);
  Leave(p);
}

TEST_CASE("A trap's world body joins the stream by the impulse touch alone: "
          "the firing seat's claim is granted, a second firing's is refused "
          "E_PROP_MOVING and answered and a contact E_PROP_KIND, the owner's "
          "samples move the record, and its rest, saved at once, returns the "
          "body to the host with the word",
          "[Prop][Traps][espm]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kByTheRocks, kEmbershard);
  Player(p, 1, kC2, kByTheRocks + NiPoint3{ 20.f, 0.f, 0.f }, kEmbershard);
  CellLoaded(p, 0, kEmbershard);
  REQUIRE(p.GetCellHost(kEmbershard).player == kC1);
  // the rock stands loaded with the chunks around the players (an impulse's
  // own lookup loads nothing, the next case's)
  auto& rock = p.worldState.GetFormAt<MpObjectReference>(kRock);
  REQUIRE(rock.GetBaseType() == "ACTI");
  REQUIRE(p.worldState.CellOf(rock) == std::optional<uint32_t>(kEmbershard));
  REQUIRE(rock.GetActorListeners().size() >= 2);
  const NiPoint3 hung = rock.GetPos();
  p.Messages().clear();

  // c2's firing set the rock moving: its impulse claim is granted, and both
  // seats hear the owner word
  Claim(p, 1, kRock, 3);
  REQUIRE(p.PropOwnerOf(rock) == kC2);
  REQUIRE(p.PropEpochOf(rock) == 1);
  const auto toC1 = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(toC1.size() == 1);
  REQUIRE(toC1[0].refr == kRock);
  REQUIRE(toC1[0].player == kC2);
  REQUIRE(toC1[0].epoch == 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).size() == 1);

  // c1's own copy of the same firing claims it too: the first firing wins
  // by A3's moving row, and c1 hears the current word; a contact never
  // claims a world body
  p.Messages().clear();
  {
    LogCapture log;
    Claim(p, 0, kRock, 3);
    Claim(p, 0, kRock, 0);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_MOVING: ff000d01 claims "
                      "prop b6d45 by touch 3; another seat streams it awake: "
                      "a contact, a drop or an impulse takes a prop at rest, "
                      "a grab takes it at once") == 1);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_KIND: ff000d01 claims "
                      "prop b6d45 by touch 0; a world body, which only an "
                      "impulse claims") == 1);
  }
  REQUIRE(p.PropOwnerOf(rock) == kC2);
  REQUIRE(p.PropEpochOf(rock) == 1);
  const auto answered = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(answered.size() == 1);
  REQUIRE(answered[0].player == kC2);
  REQUIRE(answered[0].epoch == 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());

  // the fall: the host's sample is refused (not the owner), the owner's
  // moves the record and reaches c1 alone, unreliable while awake
  p.Messages().clear();
  const NiPoint3 falling = hung + NiPoint3{ 0.f, 0.f, -100.f };
  Sample(p, 0, kRock, falling, false);
  REQUIRE(rock.GetPos() == hung);
  Sample(p, 1, kRock, falling, false);
  REQUIRE(rock.GetPos() == falling);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 0 });
  REQUIRE(RelaysTo(p, 1).empty());
  REQUIRE(rock.HasUnsavedEdit());

  // the rest: relayed reliable, the record saved at once, and the rock the
  // host's again at epoch 2, both seats told with player 0 (A4, ADR-071
  // decision 8: the traps under the one rule)
  p.Messages().clear();
  const NiPoint3 rested = falling + NiPoint3{ 10.f, 0.f, -100.f };
  Sample(p, 1, kRock, rested, true);
  REQUIRE(rock.GetPos() == rested);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 1 });
  REQUIRE(!rock.HasUnsavedEdit());
  REQUIRE(p.PropOwnerOf(rock) == kC1);
  REQUIRE(p.PropEpochOf(rock) == 2);
  const auto rest1 = SentTo<PropOwnerMessage>(p, 0);
  const auto rest2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(rest1.size() == 1);
  REQUIRE(rest2.size() == 1);
  REQUIRE(rest1[0].player == 0);
  REQUIRE(rest2[0].player == 0);
  REQUIRE(rest2[0].epoch == 2);

  // a new firing takes the resting rock at the next epoch
  p.Messages().clear();
  Claim(p, 0, kRock, 3);
  REQUIRE(p.PropOwnerOf(rock) == kC1);
  REQUIRE(p.PropEpochOf(rock) == 3);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).size() == 1);

  // a world body no impulse claimed takes no sample, the host's neither (the
  // refusal's line is once per 10 s per seat, so its effect is read here)
  auto& sibling = p.worldState.GetFormAt<MpObjectReference>(kSiblingRock);
  const NiPoint3 siblingAt = sibling.GetPos();
  REQUIRE(p.PropEpochOf(sibling) == 0);
  p.Messages().clear();
  Sample(p, 0, kSiblingRock, siblingAt + NiPoint3{ 0.f, 0.f, -50.f }, false);
  REQUIRE(sibling.GetPos() == siblingAt);
  REQUIRE(RelaysTo(p, 1).empty());
  Leave(p);
}

// the review of 9ff1895e (finding 4): an impulse's claim looked its reference
// up with a load, so the id a seat chose was loaded for a claim the server
// then refused. Bleak Falls Temple 0x371de's oil lamp 0x9b0fb (an ACTI,
// unit/DestructionStageTest's) stands in another cell than the mine
TEST_CASE("An impulse's claim loads nothing: a world body the chunks around "
          "the claimant have not loaded is refused E_PROP_UNKNOWN and stays "
          "unloaded, and one they have is granted",
          "[Prop][Traps][espm]")
{
  constexpr uint32_t kTempleLamp = 0x9b0fb;
  auto& p = GetPartOne();
  Player(p, 0, kC1, kByTheRocks, kEmbershard);
  Player(p, 1, kC2, kByTheRocks + NiPoint3{ 20.f, 0.f, 0.f }, kEmbershard);
  CellLoaded(p, 0, kEmbershard);
  REQUIRE(p.GetCellHost(kEmbershard).player == kC1);
  p.Messages().clear();

  REQUIRE(p.worldState.LookupFormByIdNoLoad(kTempleLamp) == nullptr);
  {
    LogCapture log;
    Claim(p, 1, kTempleLamp, 3);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_UNKNOWN: ff000d02 claims "
                      "prop 9b0fb by touch 3; no such reference, or an "
                      "actor") == 1);
  }
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kTempleLamp) == nullptr);
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());

  // the mine's rock stands within the claimant's grid squares, so the
  // claimant's own grid pass (the 3x3 squares around it) loaded it: the
  // impulse finds it without a load and is granted
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kRock) != nullptr);
  Claim(p, 1, kRock, 3);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).size() == 1);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1)[0].refr == kRock);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1)[0].player == kC2);
  Leave(p);
}

// the review of 9ff1895e (finding 2): "a trap's world body" admits almost
// every world object, so a crafted seat could claim one by the impulse and
// carry it off for good, a step budget at a time; a world body's sample now
// lands within the tether (2048 units, UNMEASURED: a fall's extent) of its
// game-file position or is refused E_PROP_FAR
TEST_CASE("A trap's world body takes no sample farther than the tether from "
          "its game-file position, the record standing; a sample within it "
          "is taken",
          "[Prop][Traps][espm]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kByTheRocks, kEmbershard);
  Player(p, 1, kC2, kByTheRocks + NiPoint3{ 20.f, 0.f, 0.f }, kEmbershard);
  CellLoaded(p, 0, kEmbershard);
  REQUIRE(p.GetCellHost(kEmbershard).player == kC1);
  // the rock loads where its record places it: its game-file position
  auto& rock = p.worldState.GetFormAt<MpObjectReference>(kRock);
  const NiPoint3 placed = rock.GetPos();
  Claim(p, 1, kRock, 3);
  REQUIRE(p.PropOwnerOf(rock) == kC2);
  // the record stands 2000 units below the place already (an earlier fall's
  // samples), so each sample here is a short step inside the step budget
  // and the tether alone judges it
  const NiPoint3 low = placed + NiPoint3{ 0.f, 0.f, -2000.f };
  rock.SetPos(low);
  p.Messages().clear();

  // 2100 below the place, 100 from the record: past the tether, refused, the
  // record standing and nothing relayed (the refusal's line is once per 10 s
  // per seat, so its effect is read here)
  Sample(p, 1, kRock, placed + NiPoint3{ 0.f, 0.f, -2100.f }, false);
  REQUIRE(rock.GetPos() == low);
  REQUIRE(RelaysTo(p, 0).empty());
  REQUIRE(RelaysTo(p, 1).empty());

  // 2040 below the place, 40 from the record: inside the tether, taken and
  // relayed to the host's game
  const NiPoint3 inside = placed + NiPoint3{ 0.f, 0.f, -2040.f };
  Sample(p, 1, kRock, inside, false);
  REQUIRE(rock.GetPos() == inside);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 0 });
  Leave(p);
}

TEST_CASE("A sample at a point in no cell is refused E_PROP_OUTSIDE whoever "
          "sends it, a live owner included, and a resting prop in a cell "
          "nobody hosts is refused E_PROP_FROZEN; the record stands either "
          "way",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  // seats of their own: OnPropTransform says a refusal once per 10 s per
  // seat, so these lines are read from seats no other case refused
  auto& c5 = Player(p, 5, kC5, kHere + NiPoint3{ 40.f, 0.f, 0.f });
  auto& c6 = Player(p, 6, kC6, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  auto& prop = Prop(p, kBasket, kHere);
  const auto cell = p.worldState.CellOf(prop);
  REQUIRE(cell.has_value());
  // no load report: nobody hosts the cell
  REQUIRE_FALSE(p.GetCellHost(*cell).hosted);
  REQUIRE(Listens(prop, c5));
  REQUIRE(Listens(prop, c6));
  // a point past the world's cells: grid square (219, 0), which Tamriel
  // carries no cell at (the premise, read here, not assumed)
  const NiPoint3 nowhere{ 900000.f, 500.f, 0.f };
  REQUIRE_FALSE(p.worldState.CellOf(kTamriel, nowhere).has_value());
  p.Messages().clear();
  {
    LogCapture log;
    // the resting prop in the unhosted cell freezes at its record
    Sample(p, 5, kBasket, kHere + NiPoint3{ 10.f, 0.f, 0.f }, false);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_FROZEN: ff000d09 moves "
                      "prop ff000d03; a resting prop in a cell nobody "
                      "hosts") == 1);
    // a point in no cell: refused before any owner is asked
    Sample(p, 6, kBasket, nowhere, false);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_OUTSIDE: ff000d0a moves "
                      "prop ff000d03; no cell at the sample's point") == 1);
  }
  REQUIRE(prop.GetPos() == kHere);
  REQUIRE(RelaysTo(p, 5).empty());
  REQUIRE(RelaysTo(p, 6).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 5).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 6).empty());

  // a toucher's live row streams in the unhosted cell (a moving body's
  // owner never depends on its cell), but never past the world's cells
  Claim(p, 5, kBasket, 0);
  REQUIRE(p.PropLiveOwner(prop) == kC5);
  const NiPoint3 kicked = kHere + NiPoint3{ 10.f, 0.f, 0.f };
  Sample(p, 5, kBasket, kicked, false);
  REQUIRE(prop.GetPos() == kicked);
  p.Messages().clear();
  Sample(p, 5, kBasket, nowhere, false);
  REQUIRE(prop.GetPos() == kicked);
  REQUIRE(RelaysTo(p, 6).empty());
  REQUIRE(p.PropLiveOwner(prop) == kC5);

  DoDisconnect(p, 5);
  DoDisconnect(p, 6);
  p.DestroyActor(kC5);
  p.DestroyActor(kC6);
}
