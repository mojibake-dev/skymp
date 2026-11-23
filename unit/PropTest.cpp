#include "ActionListener.h"
#include "ActivateMessage.h"
#include "ActorValueRecord.h"
#include "CreateActorMessage.h"
#include "DestroyActorMessage.h"
#include "DestructionStageMessage.h"
#include "HitMessage.h"
#include "PropClaimMessage.h"
#include "PropOwnerMessage.h"
#include "PropTransformMessage.h"
#include "TestUtils.hpp"
#include "libespm/Loader.h"
#include "libespm/REFR.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <cmath>
#include <limits>
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
// Physics rung 2 (ADR-071 decision 6): a sample names the epoch its sender
// holds, a sequence within it and its owner's clock, and is judged by the
// epoch first; a refused sample is answered with the word and the record
// (PropTransform at seq 0); a rest past the step budget is clamped; a claim
// names one to sixteen props, each judged alone. The owner's-clock cases of
// the step budget are the Rust rule's too. Physics rung 4 (Eli's D-PH9 A and
// D-PH10 A): the server holds every movable static, a world body is claimed
// by any touch and woken by its host as an item is, and its sample carries
// its parts as bones, relayed and never recorded; the body's class is each
// seat's, so no case here reads one.

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
// physics rung 2's seats of their own (a refusal is said once per 10 s per
// seat, a sample's and a claim's alike; the time is the connection's, so a
// case's own connects start it afresh) and two more props
constexpr uint32_t kC7 = 0xff000d0c;
constexpr uint32_t kC8 = 0xff000d0d;
constexpr uint32_t kJug = 0xff000d0e;
constexpr uint32_t kCrate = 0xff000d0f;
constexpr uint32_t kC9 = 0xff000d10;
constexpr uint32_t kC10 = 0xff000d11;
constexpr uint32_t kC11 = 0xff000d12;
constexpr uint32_t kC12 = 0xff000d13;
constexpr uint32_t kC13 = 0xff000d14;
// physics rung 3a's seats of their own, for its refusal line
constexpr uint32_t kC14 = 0xff000d15;
constexpr uint32_t kC15 = 0xff000d16;
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

// `user`'s touch of the props `refrs` in one claim (physics rung 2: one to
// sixteen, each judged alone)
void ClaimAll(PartOne& p, Networking::UserId user,
              const std::vector<uint32_t>& refrs, uint32_t touch)
{
  PropClaimMessage msg;
  msg.refrs = refrs;
  msg.touch = touch;
  p.GetActionListener().OnPropClaim(Raw(user), msg);
}

// `user`'s touch of the prop `refr`
void Claim(PartOne& p, Networking::UserId user, uint32_t refr, uint32_t touch)
{
  ClaimAll(p, user, { refr }, touch);
}

// the next sample number of this file's senders: one counter for every
// sample, so each is newer than the last at its epoch (the rule's order),
// wrapping 65535 to 1 as a client does
uint16_t NextSeq()
{
  static uint16_t seq = 0;
  seq = seq == 65535 ? 1 : static_cast<uint16_t>(seq + 1);
  return seq;
}

// a sample of the prop `refr` as an honest seat sends it (physics rung 2):
// under the epoch the prop stands at (the seat heard every word), the next
// number, the owner's clock 100 ms on a number (the client's cadence), no
// velocity, no hold and no bones
PropTransformMessage SampleOf(PartOne& p, uint32_t refr, const NiPoint3& pos,
                              bool asleep)
{
  PropTransformMessage msg;
  msg.refr = refr;
  const auto& form = p.worldState.LookupFormByIdNoLoad(refr);
  const MpObjectReference* prop = form ? form->AsObjectReference() : nullptr;
  msg.epoch = prop ? p.PropEpochOf(*prop) : 0;
  msg.seq = NextSeq();
  msg.tMs = static_cast<uint32_t>(msg.seq) * 100;
  msg.pos = { pos.x, pos.y, pos.z };
  msg.rot = { 0.f, 0.f, 72.f };
  msg.asleep = asleep;
  return msg;
}

// `user`'s sample of the prop `refr`
void Sample(PartOne& p, Networking::UserId user, uint32_t refr,
            const NiPoint3& pos, bool asleep)
{
  p.GetActionListener().OnPropTransform(Raw(user),
                                        SampleOf(p, refr, pos, asleep));
}

// `user`'s sample under an epoch and a number of its own: a stale or a
// future one, a repeat
void SampleAt(PartOne& p, Networking::UserId user, uint32_t refr,
              const NiPoint3& pos, bool asleep, uint32_t epoch, uint16_t seq)
{
  auto msg = SampleOf(p, refr, pos, asleep);
  msg.epoch = epoch;
  msg.seq = seq;
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

// where in the sent messages the first of type Message to `user` stands,
// -1 for none: the order of a word and a record on the one channel
template <class Message>
int FirstTo(PartOne& p, Networking::UserId user)
{
  int at = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == user && dynamic_cast<Message*>(m.message.get())) {
      return at;
    }
    ++at;
  }
  return -1;
}

// how many DestroyActors for `refr` went to `user`: its game told the
// reference is gone (an unsubscription; SpellPlacedTest's DestroyedTo shape,
// by the reference's idx)
size_t DestroysTo(PartOne& p, Networking::UserId user, MpObjectReference& refr)
{
  size_t n = 0;
  for (auto& m : p.Messages()) {
    auto destroy = dynamic_cast<DestroyActorMessage*>(m.message.get());
    if (m.userId == user && destroy && destroy->idx == refr.GetIdx()) {
      ++n;
    }
  }
  return n;
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

// physics rung 4 (thuum ADR-071; Eli's D-PH9 A and D-PH10 A): the Riverwood
// Trader's sign 0x5161a, a REFR of the movable static (MSTT)
// SignRiverwoodRiverwoodTrader01 0x38229 in Tamriel 0x3c at (21739.4,
// -45609.9, 127.5), record flags 0 and no enable parent (lab/esm.py ref on
// Skyrim.esm, 2026-11-13; no override in Update.esm or the three DLCs):
// seven bodies on a Fixed anchor in the game, one reference to the server,
// whose positions here are read from its record and its form, never typed
constexpr uint32_t kSign = 0x5161a;
constexpr uint32_t kSignBase = 0x38229;

// where the game files place `refr` (its REFR record's DATA), read before
// anything loads it: the players stand by a reference the server holds only
// once their own grid pass loads its chunk
NiPoint3 PlacedAt(PartOne& p, uint32_t refr)
{
  const auto data = espm::GetData<espm::REFR>(refr, &p.worldState);
  REQUIRE(data.loc);
  return { data.loc->pos[0], data.loc->pos[1], data.loc->pos[2] };
}

// two players beside the sign, c1 hosting its cell (Enter's shape); the sign
// is the chunk's, loaded by the players' grid pass, never by its id
World EnterBySign(PartOne& p)
{
  const NiPoint3 placed = PlacedAt(p, kSign);
  Player(p, 0, kC1, placed + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 1, kC2, placed + NiPoint3{ 60.f, 0.f, 0.f });
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSign) != nullptr);
  auto& sign = p.worldState.GetFormAt<MpObjectReference>(kSign);
  const auto cell = p.worldState.CellOf(sign);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(sign.GetActorListeners().size() >= 2);
  p.Messages().clear();
  return World{ p, sign, *cell };
}

// `n` part poses about `pos`, as a seat's walk lists a body's parts: each
// within 512 units of the sample's position (wire-validate's reach, the
// edge's; the legacy path here has none), its quaternion unit length
std::vector<PropTransformMessage::Bone> BonesAbout(const NiPoint3& pos,
                                                   size_t n)
{
  std::vector<PropTransformMessage::Bone> bones(n);
  for (size_t i = 0; i < n; ++i) {
    bones[i].pos = { pos.x + 10.f * static_cast<float>(i % 16),
                     pos.y + 5.f * static_cast<float>(i / 16), pos.z - 40.f };
    // an eighth of a turn about z: (0, 0, sin 22.5 deg, cos 22.5 deg)
    bones[i].quat = { 0.f, 0.f, 0.38268343f, 0.92387953f };
  }
  return bones;
}

// `user`'s sample of `refr` carrying `n` bones about `pos` (SampleOf's
// honest seat otherwise)
PropTransformMessage SampleWithBones(PartOne& p, uint32_t refr,
                                     const NiPoint3& pos, bool asleep,
                                     size_t n)
{
  auto msg = SampleOf(p, refr, pos, asleep);
  msg.bones = BonesAbout(pos, n);
  return msg;
}

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}
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
  // and the record after the word (physics rung 2: every refusal answered
  // with the record), at seq 0, where the record stands
  const auto record = SentTo<PropTransformMessage>(p, 0);
  REQUIRE(record.size() == 1);
  REQUIRE(record[0].seq == 0);
  REQUIRE(record[0].epoch == 1);
  REQUIRE(record[0].pos == std::array<float, 3>{ kHere.x, kHere.y, kHere.z });
  REQUIRE_FALSE(record[0].asleep);
  REQUIRE(FirstTo<PropOwnerMessage>(p, 0) <
          FirstTo<PropTransformMessage>(p, 0));

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
          "budget is refused, the record standing, and an asleep one is "
          "clamped to the budget with the word; a late listener hears the "
          "current word, player 0 for a resting prop",
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

  // the step budget: the owner's sample from across the cell, past even an
  // epoch's first sample's half second at the cap (3,000 units, physics rung
  // 3a), leaves the record where it stands and reaches nobody; a near sample
  // passes
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

  // an asleep sample over the budget is clamped (physics rung 2, in rung
  // 1's RestRefused's place): the record moves the budget's 600 units toward
  // it (the near sample, its epoch's first, left one step, physics rung 3a,
  // and a later sample's allowance holds one step at most),
  // the other listeners hear that rest re-encoded, not the sender's bytes,
  // and the rest is taken: the host's again at the next epoch, every
  // listener told with player 0, the sender the record
  const auto epochAtFlight = p.PropEpochOf(w.prop);
  p.Messages().clear();
  Sample(p, 1, kBasket, kHere + NiPoint3{ 5000.f, 0.f, 0.f }, true);
  REQUIRE(std::abs(w.prop.GetPos().x - (near.x + 600.f)) < 1.f);
  REQUIRE(RelaysTo(p, 0).empty());
  const auto clamped = SentTo<PropTransformMessage>(p, 0);
  REQUIRE(clamped.size() == 1);
  REQUIRE(clamped[0].asleep);
  REQUIRE(std::abs(clamped[0].pos[0] - (near.x + 600.f)) < 1.f);
  const auto record = SentTo<PropTransformMessage>(p, 1);
  REQUIRE(record.size() == 1);
  REQUIRE(record[0].seq == 0);
  REQUIRE(record[0].epoch == epochAtFlight + 1);
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
  // told with player 0
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
  // c2's late sample is refused Away too and not answered (physics rung 2
  // judges the unload's state before the owner: its game's copy went with
  // the cell); its line is not said, the seat's second refusal inside 10 s
  // (a seat's refused samples and claims share one line a 10 s, the review
  // of 3892415e, finding 4)
  p.Messages().clear();
  {
    LogCapture log;
    Sample(p, 1, kBasket, kHere + NiPoint3{ 40.f, 0.f, 0.f }, false);
    REQUIRE(log.Count("E_PROP_AWAY") == 0);
  }
  REQUIRE(std::abs(w.prop.GetPos().x - (kHere.x + 20.f)) < 1.f);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  REQUIRE(SentTo<PropTransformMessage>(p, 1).empty());
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

TEST_CASE("A trap body's first firing still wins: the firing seat's claim is "
          "granted, a second firing's and a contact's are refused "
          "E_PROP_MOVING and answered, the owner's samples move the record, "
          "and its rest, saved at once, returns the body to the host with "
          "the word",
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

  // since physics rung 4 a contact claims a world body as it claims an item
  // (Eli's D-PH9 A; it was E_PROP_KIND), so c1's walk into the falling rock
  // is A3's moving row like c1's own copy of the same firing: the first
  // firing wins, c1 hears the current word once (below, once per prop per
  // second), and the impulse's refusal line is not said, the seat's second
  // inside 10 s (a seat's refused samples and claims share one line a 10 s,
  // the review of 3892415e, finding 4)
  p.Messages().clear();
  {
    LogCapture log;
    Claim(p, 0, kRock, 0);
    Claim(p, 0, kRock, 3);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_MOVING: ff000d01 claims "
                      "prop b6d45 by touch 0; another seat streams it awake: "
                      "a contact, a drop or an impulse takes a prop at rest, "
                      "a grab takes it at once") == 1);
    REQUIRE(log.Count("E_PROP_MOVING") == 1);
    REQUIRE(log.Count("E_PROP_KIND") == 0);
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
  Leave(p);
}

// physics rung 4 (DEF-4a): the epoch-0 refusal of D-S3 point 4 ("a world
// body no impulse claimed") retires, so the host's own engine moving a
// resting world body is A7's wake, as an item's is ("The host's own sample
// of a resting prop wakes it"); this case takes the trap case's old last
// step, where the same sample was refused
TEST_CASE("The host's own sample of a resting world body wakes it: the row "
          "the host's at epoch 1, the sample relayed, and the word names the "
          "host by id (A7)",
          "[Prop][Traps][espm]")
{
  auto& p = GetPartOne();
  auto& c1 = Player(p, 0, kC1, kByTheRocks, kEmbershard);
  Player(p, 1, kC2, kByTheRocks + NiPoint3{ 20.f, 0.f, 0.f }, kEmbershard);
  CellLoaded(p, 0, kEmbershard);
  REQUIRE(p.GetCellHost(kEmbershard).player == kC1);
  // the sibling rock stands loaded with the chunks around the players, at
  // rest and untouched, the host's by default
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSiblingRock) != nullptr);
  auto& sibling = p.worldState.GetFormAt<MpObjectReference>(kSiblingRock);
  REQUIRE(sibling.GetBaseType() == "ACTI");
  REQUIRE(Listens(sibling, c1));
  const NiPoint3 siblingAt = sibling.GetPos();
  REQUIRE(p.PropEpochOf(sibling) == 0);
  REQUIRE(p.PropLiveOwner(sibling) == 0);
  p.Messages().clear();

  const NiPoint3 kicked = siblingAt + NiPoint3{ 0.f, 0.f, -50.f };
  {
    LogCapture log;
    Sample(p, 0, kSiblingRock, kicked, false);
    REQUIRE(log.Count("[verb:prop-physics] prop b6d48 woken by its host "
                      "ff000d01 at (") == 1);
    REQUIRE(log.Count("E_PROP_") == 0);
  }
  REQUIRE(sibling.GetPos() == kicked);
  REQUIRE(p.PropOwnerOf(sibling) == kC1);
  REQUIRE(p.PropLiveOwner(sibling) == kC1);
  REQUIRE(p.PropEpochOf(sibling) == 1);
  REQUIRE(RelaysTo(p, 1) == std::vector<int>{ 0 });
  for (const Networking::UserId user :
       std::vector<Networking::UserId>{ 0, 1 }) {
    const auto words = SentTo<PropOwnerMessage>(p, user);
    REQUIRE(words.size() == 1);
    REQUIRE(words[0].refr == kSiblingRock);
    REQUIRE(words[0].player == kC1);
    REQUIRE(words[0].epoch == 1);
  }
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
                      "prop 9b0fb by touch 3; no such reference") == 1);
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
  // seats of their own: a refusal is said once per 10 s per seat, so each
  // line is read from a seat no step of this case refused before
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
  // each sender heard the word (nobody's: player 0) and the record where it
  // stands (physics rung 2: every refusal answered with the record)
  const Networking::UserId senders[] = { 5, 6 };
  for (const auto user : senders) {
    const auto words = SentTo<PropOwnerMessage>(p, user);
    REQUIRE(words.size() == 1);
    REQUIRE(words[0].player == 0);
    const auto records = SentTo<PropTransformMessage>(p, user);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].seq == 0);
    REQUIRE(records[0].pos ==
            std::array<float, 3>{ kHere.x, kHere.y, kHere.z });
    REQUIRE(records[0].asleep);
  }

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

TEST_CASE("A sample names its epoch: a stale one and a future one are "
          "dropped, never relayed, and answered with the word and then the "
          "record, once a second",
          "[Prop][espm]")
{
  // physics rung 2 (ADR-066 decision 4): by the epoch first, then the owner;
  // seats of their own, for the refusal lines
  auto& p = GetPartOne();
  Player(p, 7, kC7, kHere + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 8, kC8, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  auto& prop = Prop(p, kBasket, kHere);
  const auto cell = p.worldState.CellOf(prop);
  REQUIRE(cell.has_value());
  CellLoaded(p, 7, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC7);
  // c7's contact at 1, c8's grab of the moving prop at 2
  Claim(p, 7, kBasket, 0);
  Claim(p, 8, kBasket, 1);
  REQUIRE(p.PropLiveOwner(prop) == kC8);
  REQUIRE(p.PropEpochOf(prop) == 2);
  p.Messages().clear();
  {
    LogCapture log;
    // c7's sample under epoch 1, sent before the grab's word reached it:
    // stale; c8's under epoch 3, a word the server never said: future
    SampleAt(p, 7, kBasket, kHere + NiPoint3{ 30.f, 0.f, 0.f }, false, 1, 1);
    SampleAt(p, 8, kBasket, kHere + NiPoint3{ 30.f, 0.f, 0.f }, false, 3, 1);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_STALE: ff000d0c moves prop "
                      "ff000d03; an epoch below the prop's: a sample of an "
                      "earlier tenure") == 1);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_FUTURE: ff000d0d moves "
                      "prop ff000d03; an epoch above the prop's, a word the "
                      "server never said") == 1);
  }
  // nothing moved and nothing was relayed
  REQUIRE(prop.GetPos() == kHere);
  REQUIRE(RelaysTo(p, 7).empty());
  REQUIRE(RelaysTo(p, 8).empty());
  // each sender heard the word (c8's, at 2), then the record at seq 0: the
  // prop held, so awake
  const Networking::UserId senders[] = { 7, 8 };
  for (const auto user : senders) {
    const auto words = SentTo<PropOwnerMessage>(p, user);
    REQUIRE(words.size() == 1);
    REQUIRE(words[0].player == kC8);
    REQUIRE(words[0].epoch == 2);
    const auto records = SentTo<PropTransformMessage>(p, user);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].refr == kBasket);
    REQUIRE(records[0].seq == 0);
    REQUIRE(records[0].tMs == 0);
    REQUIRE(records[0].epoch == 2);
    REQUIRE(records[0].pos ==
            std::array<float, 3>{ kHere.x, kHere.y, kHere.z });
    REQUIRE_FALSE(records[0].asleep);
    REQUIRE(records[0].held);
    REQUIRE(FirstTo<PropOwnerMessage>(p, user) <
            FirstTo<PropTransformMessage>(p, user));
  }
  // a second refusal inside the second is not answered again
  SampleAt(p, 7, kBasket, kHere + NiPoint3{ 30.f, 0.f, 0.f }, false, 1, 2);
  REQUIRE(SentTo<PropOwnerMessage>(p, 7).size() == 1);
  REQUIRE(SentTo<PropTransformMessage>(p, 7).size() == 1);
  // c8's sample under its grab's epoch is taken and relayed
  Sample(p, 8, kBasket, kHere + NiPoint3{ 30.f, 0.f, 0.f }, false);
  REQUIRE(std::abs(prop.GetPos().x - (kHere.x + 30.f)) < 1.f);
  REQUIRE(RelaysTo(p, 7) == std::vector<int>{ 0 });
  DoDisconnect(p, 7);
  DoDisconnect(p, 8);
  p.DestroyActor(kC7);
  p.DestroyActor(kC8);
}

TEST_CASE("A woken host's own sample of the tenure its wake ended is dropped "
          "and not answered (its word is on its way and its body ahead of "
          "the record); another seat's of that tenure is answered",
          "[Prop][espm]")
{
  // the plan's R5 (no grace, ADR-066 decision 5 not built): the host's wake
  // raises the epoch, and the samples it sent before the wake's word reached
  // it are stale; a record answer would rewind the body it simulates
  auto& p = GetPartOne();
  Player(p, 12, kC12, kHere + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 13, kC13, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  auto& prop = Prop(p, kBasket, kHere);
  const auto cell = p.worldState.CellOf(prop);
  REQUIRE(cell.has_value());
  CellLoaded(p, 12, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC12);
  // the host's kick of the untouched prop wakes it at 1, by name
  SampleAt(p, 12, kBasket, kHere + NiPoint3{ 10.f, 0.f, 0.f }, false, 0, 1);
  REQUIRE(p.PropLiveOwner(prop) == kC12);
  REQUIRE(p.PropEpochOf(prop) == 1);
  p.Messages().clear();
  {
    LogCapture log;
    // its next sample, sent under epoch 0 before the word came back: stale,
    // counted and said, not answered, the record standing
    SampleAt(p, 12, kBasket, kHere + NiPoint3{ 20.f, 0.f, 0.f }, false, 0, 2);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_STALE: ff000d13 moves prop "
                      "ff000d03; an epoch below the prop's: a sample of an "
                      "earlier tenure") == 1);
  }
  REQUIRE(std::abs(prop.GetPos().x - (kHere.x + 10.f)) < 1.f);
  REQUIRE(RelaysTo(p, 13).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 12).empty());
  REQUIRE(SentTo<PropTransformMessage>(p, 12).empty());
  // the other seat's sample of that tenure is answered: the host's word at
  // 1, then the record
  SampleAt(p, 13, kBasket, kHere + NiPoint3{ 20.f, 0.f, 0.f }, false, 0, 1);
  const auto words = SentTo<PropOwnerMessage>(p, 13);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].player == kC12);
  REQUIRE(words[0].epoch == 1);
  const auto records = SentTo<PropTransformMessage>(p, 13);
  REQUIRE(records.size() == 1);
  REQUIRE(records[0].seq == 0);
  REQUIRE(std::abs(records[0].pos[0] - (kHere.x + 10.f)) < 1.f);
  // and the host's samples under its wake's epoch go on
  Sample(p, 12, kBasket, kHere + NiPoint3{ 20.f, 0.f, 0.f }, false);
  REQUIRE(std::abs(prop.GetPos().x - (kHere.x + 20.f)) < 1.f);
  REQUIRE(RelaysTo(p, 13) == std::vector<int>{ 0 });
  DoDisconnect(p, 12);
  DoDisconnect(p, 13);
  p.DestroyActor(kC12);
  p.DestroyActor(kC13);
}

TEST_CASE("A repeated or overtaken sequence is dropped quietly: no relay, no "
          "line and no answer",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  Player(p, 9, kC9, kHere + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 10, kC10, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  auto& prop = Prop(p, kBasket, kHere);
  Claim(p, 9, kBasket, 0);
  REQUIRE(p.PropLiveOwner(prop) == kC9);
  p.Messages().clear();
  {
    LogCapture log;
    // seq 2 twice, then 1: the first taken and relayed, the others Late
    SampleAt(p, 9, kBasket, kHere + NiPoint3{ 20.f, 0.f, 0.f }, false, 1, 2);
    SampleAt(p, 9, kBasket, kHere + NiPoint3{ 25.f, 0.f, 0.f }, false, 1, 2);
    SampleAt(p, 9, kBasket, kHere + NiPoint3{ 25.f, 0.f, 0.f }, false, 1, 1);
    REQUIRE(log.Count("E_PROP_") == 0);
  }
  REQUIRE(std::abs(prop.GetPos().x - (kHere.x + 20.f)) < 1.f);
  REQUIRE(RelaysTo(p, 10) == std::vector<int>{ 0 });
  REQUIRE(SentTo<PropOwnerMessage>(p, 9).empty());
  REQUIRE(SentTo<PropTransformMessage>(p, 9).empty());
  DoDisconnect(p, 9);
  DoDisconnect(p, 10);
  p.DestroyActor(kC9);
  p.DestroyActor(kC10);
}

TEST_CASE("One claim of three props judges each: two granted with their "
          "words, the third held by another seat and answered, a line each",
          "[Prop][espm]")
{
  // physics rung 2 (ADR-071 decision 6): an impact's bodies in one claim,
  // each judged alone (here an impulse's, touch 3, on three items)
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& jug = Prop(p, kJug, kHere + NiPoint3{ 20.f, 0.f, 0.f });
  auto& crate = Prop(p, kCrate, kHere + NiPoint3{ -20.f, 0.f, 0.f });
  // c2 holds the jug
  Claim(p, 1, kJug, 1);
  REQUIRE(p.PropLiveOwner(jug) == kC2);
  p.Messages().clear();
  {
    LogCapture log;
    ClaimAll(p, 0, { kBasket, kJug, kCrate }, 3);
    REQUIRE(log.Count("[verb:prop-physics] ff000d01 claims prop ff000d03 by "
                      "touch 3: granted, epoch 1; 2 listener(s) told") == 1);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_HELD: ff000d01 claims prop "
                      "ff000d0e by touch 3; another player holds it "
                      "grabbed") == 1);
    REQUIRE(log.Count("[verb:prop-physics] ff000d01 claims prop ff000d0f by "
                      "touch 3: granted, epoch 1; 2 listener(s) told") == 1);
  }
  REQUIRE(p.PropLiveOwner(w.prop) == kC1);
  REQUIRE(p.PropLiveOwner(crate) == kC1);
  REQUIRE(p.PropLiveOwner(jug) == kC2);
  REQUIRE(p.PropEpochOf(jug) == 1);
  // c1 heard the two grants and the jug's answer (c2's word); c2 the two
  // grants alone
  const auto toC1 = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(toC1.size() == 3);
  const auto toC2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(toC2.size() == 2);
  for (const auto& word : toC1) {
    REQUIRE(word.player == (word.refr == kJug ? kC2 : kC1));
  }
  for (const auto& word : toC2) {
    REQUIRE(word.refr != kJug);
    REQUIRE(word.player == kC1);
  }
  Leave(p);
}

TEST_CASE("A rest past the step budget is clamped: the record moves the "
          "budget's distance toward it, the other seat hears that rest "
          "re-encoded, both hear the host's word and the sender the record",
          "[Prop][espm]")
{
  // ADR-071 decision 6: a live owner's rest past the bucket is clamped and
  // accepted. Right after c1's grant its prop holds its first sample's whole
  // allowance, half a second at the cap, 3,000 units (physics rung 3a; one
  // step, 600, before it), so the rest stands 3,500 off (1,000 before)
  auto& p = GetPartOne();
  auto w = Enter(p);
  Claim(p, 0, kBasket, 0);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  p.Messages().clear();
  {
    LogCapture log;
    Sample(p, 0, kBasket, kHere + NiPoint3{ 3500.f, 0.f, 0.f }, true);
    REQUIRE(log.Count("[verb:prop-physics] prop ff000d03 rests on "
                      "ff000d01's screen at (12000, 500, 0): the host's again "
                      "at epoch 2; the sample relayed to 1 listener(s), the "
                      "word to 2, clamped from (12500, 500, 0)") == 1);
  }
  REQUIRE(std::abs(w.prop.GetPos().x - (kHere.x + 3000.f)) < 1.f);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  // c2 hears the rest at the clamped point, typed (never the sender's raw
  // bytes, which name the far point), then the word
  REQUIRE(RelaysTo(p, 1).empty());
  const auto relay = SentTo<PropTransformMessage>(p, 1);
  REQUIRE(relay.size() == 1);
  REQUIRE(relay[0].asleep);
  REQUIRE(relay[0].epoch == 1);
  REQUIRE(relay[0].seq != 0);
  REQUIRE(std::abs(relay[0].pos[0] - (kHere.x + 3000.f)) < 1.f);
  REQUIRE(FirstTo<PropTransformMessage>(p, 1) <
          FirstTo<PropOwnerMessage>(p, 1));
  // both seats hear the host's word at 2
  for (const Networking::UserId user :
       std::vector<Networking::UserId>{ 0, 1 }) {
    const auto words = SentTo<PropOwnerMessage>(p, user);
    REQUIRE(words.size() == 1);
    REQUIRE(words[0].player == 0);
    REQUIRE(words[0].epoch == 2);
  }
  // and c1 the record, after the word
  const auto record = SentTo<PropTransformMessage>(p, 0);
  REQUIRE(record.size() == 1);
  REQUIRE(record[0].seq == 0);
  REQUIRE(record[0].epoch == 2);
  REQUIRE(record[0].asleep);
  REQUIRE(std::abs(record[0].pos[0] - (kHere.x + 3000.f)) < 1.f);
  REQUIRE(FirstTo<PropOwnerMessage>(p, 0) <
          FirstTo<PropTransformMessage>(p, 0));
  Leave(p);
}

TEST_CASE("A claimant's first sample after its grant may stand half a second "
          "of flight from the record: taken and relayed; its next sample as "
          "far again at once is refused E_PROP_STEP and answered with the "
          "word and the record",
          "[Prop][espm]")
{
  // physics rung 3a (ADR-071's ladder): a body an impulse moves flies before
  // the word naming its mover reaches that seat, so the tenure's first sample
  // may spend half a second of the prop's step-budget clock at the cap
  // (wire-rules props.rs PROP_FIRST_SAMPLE_UNITS, 3,000 units) where a later
  // one holds one step, 600; a prop no row has held yet holds the half second
  // whole, so no clock is advanced here. c15 hosts the cell and c14 casts at
  // its prop; seats of their own, for the refusal line
  auto& p = GetPartOne();
  Player(p, 14, kC14, kHere + NiPoint3{ 40.f, 0.f, 0.f });
  auto& c15 = Player(p, 15, kC15, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  auto& prop = Prop(p, kBasket, kHere);
  const auto cell = p.worldState.CellOf(prop);
  REQUIRE(cell.has_value());
  CellLoaded(p, 15, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC15);
  REQUIRE(Listens(prop, c15));
  // c14's impact claims the resting prop: granted at epoch 1
  Claim(p, 14, kBasket, 3);
  REQUIRE(p.PropLiveOwner(prop) == kC14);
  REQUIRE(p.PropEpochOf(prop) == 1);
  p.Messages().clear();

  // its first sample, at once and 2,000 units off: the record takes it and
  // the host's game hears it, unreliable while awake; nothing answers c14
  const NiPoint3 flown = kHere + NiPoint3{ 2000.f, 0.f, 0.f };
  Sample(p, 14, kBasket, flown, false);
  REQUIRE(prop.GetPos() == flown);
  REQUIRE(RelaysTo(p, 15) == std::vector<int>{ 0 });
  REQUIRE(SentTo<PropOwnerMessage>(p, 14).empty());
  REQUIRE(SentTo<PropTransformMessage>(p, 14).empty());

  // its next sample 2,000 farther at the same moment: past what the first
  // left (one step at most) and 100 ms of its clock, refused, the record
  // standing and nothing relayed; c14 hears its own word and then the record
  p.Messages().clear();
  {
    LogCapture log;
    Sample(p, 14, kBasket, flown + NiPoint3{ 2000.f, 0.f, 0.f }, false);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_STEP: ff000d15 moves prop "
                      "ff000d03; farther from the record than the step budget "
                      "allows") == 1);
  }
  REQUIRE(prop.GetPos() == flown);
  REQUIRE(RelaysTo(p, 15).empty());
  const auto words = SentTo<PropOwnerMessage>(p, 14);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].player == kC14);
  REQUIRE(words[0].epoch == 1);
  const auto records = SentTo<PropTransformMessage>(p, 14);
  REQUIRE(records.size() == 1);
  REQUIRE(records[0].seq == 0);
  REQUIRE(records[0].epoch == 1);
  REQUIRE(records[0].pos == std::array<float, 3>{ flown.x, flown.y, flown.z });
  REQUIRE_FALSE(records[0].asleep);
  REQUIRE(FirstTo<PropOwnerMessage>(p, 14) <
          FirstTo<PropTransformMessage>(p, 14));
  REQUIRE(p.PropLiveOwner(prop) == kC14);
  DoDisconnect(p, 14);
  DoDisconnect(p, 15);
  p.DestroyActor(kC14);
  p.DestroyActor(kC15);
}

TEST_CASE("Bones on an item are refused: the sample moves nothing, wakes "
          "nothing and reaches nobody",
          "[Prop][espm]")
{
  // physics rung 2: the bones field is on the wire; since physics rung 4 a
  // world body's row takes them (its parts, D-PH10 A) and an item's never
  // does, its body being one part (a dead actor's bones are rung 5's); a
  // seat of its own, for the refusal line
  auto& p = GetPartOne();
  Player(p, 11, kC11, kHere + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 10, kC10, kHere + NiPoint3{ 60.f, 0.f, 0.f });
  auto& prop = Prop(p, kBasket, kHere);
  const auto cell = p.worldState.CellOf(prop);
  REQUIRE(cell.has_value());
  CellLoaded(p, 11, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC11);
  p.Messages().clear();
  auto msg = SampleOf(p, kBasket, kHere + NiPoint3{ 20.f, 0.f, 0.f }, false);
  PropTransformMessage::Bone bone;
  bone.pos = { kHere.x + 20.f, kHere.y, kHere.z + 30.f };
  msg.bones.push_back(bone);
  {
    LogCapture log;
    p.GetActionListener().OnPropTransform(Raw(11), msg);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_KIND: ff000d12 moves prop "
                      "ff000d03; a sample with bones on an item, whose body "
                      "is one part") == 1);
  }
  REQUIRE(prop.GetPos() == kHere);
  REQUIRE(p.PropEpochOf(prop) == 0);
  REQUIRE(RelaysTo(p, 10).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 11).empty());
  REQUIRE(SentTo<PropTransformMessage>(p, 11).empty());
  // the same sample without its bone is the host's wake
  msg.bones.clear();
  p.GetActionListener().OnPropTransform(Raw(11), msg);
  REQUIRE(p.PropLiveOwner(prop) == kC11);
  REQUIRE(p.PropEpochOf(prop) == 1);
  REQUIRE(RelaysTo(p, 10) == std::vector<int>{ 0 });
  DoDisconnect(p, 11);
  DoDisconnect(p, 10);
  p.DestroyActor(kC11);
  p.DestroyActor(kC10);
}

TEST_CASE("One impact's claim names what it moved: the resting props are the "
          "caster's at the next epoch each, one another seat streams awake "
          "is refused E_PROP_MOVING and answered with its word, and the "
          "island's next claim takes a resting neighbour the same way and "
          "is the caster's own for what it holds",
          "[Prop][espm]")
{
  // physics rung 3 (thuum lab/.cache/drafts/physics/rung3-plan.md B4; ADR-071
  // decision 4: the caster claims what its impact moved in one PropClaim per
  // impact, and the neighbours that join its island in claims of their own):
  // c1 hosts the cell and its own engine kicked the jug, which it streams
  // awake when c2's blast lands on the basket and the jug
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& jug = Prop(p, kJug, kHere + NiPoint3{ 20.f, 0.f, 0.f });
  auto& crate = Prop(p, kCrate, kHere + NiPoint3{ -20.f, 0.f, 0.f });
  Sample(p, 0, kJug, kHere + NiPoint3{ 30.f, 0.f, 0.f }, false);
  REQUIRE(p.PropLiveOwner(jug) == kC1);
  REQUIRE(p.PropEpochOf(jug) == 1);
  p.Messages().clear();
  {
    LogCapture log;
    ClaimAll(p, 1, { kBasket, kJug }, 3);
    REQUIRE(log.Count("[verb:prop-physics] ff000d02 claims prop ff000d03 by "
                      "touch 3: granted, epoch 1; 2 listener(s) told") == 1);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_MOVING: ff000d02 claims "
                      "prop ff000d0e by touch 3; another seat streams it "
                      "awake: a contact, a drop or an impulse takes a prop at "
                      "rest, a grab takes it at once") == 1);
  }
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  REQUIRE(p.PropLiveOwner(jug) == kC1);
  REQUIRE(p.PropEpochOf(jug) == 1);
  // c2 heard the basket's grant and the jug's standing word (c1's at 1); c1
  // the grant alone
  const auto toC2 = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(toC2.size() == 2);
  for (const auto& word : toC2) {
    REQUIRE(word.player == (word.refr == kJug ? kC1 : kC2));
    REQUIRE(word.epoch == 1);
  }
  const auto toC1 = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(toC1.size() == 1);
  REQUIRE(toC1[0].refr == kBasket);
  REQUIRE(toC1[0].player == kC2);

  // the basket in flight knocked the resting crate into its island: c2's
  // next claim names the crate and the basket again, the crate granted with
  // its word to both seats, the basket c2's own already (no word, no line)
  p.Messages().clear();
  {
    LogCapture log;
    ClaimAll(p, 1, { kCrate, kBasket }, 3);
    REQUIRE(log.Count("[verb:prop-physics] ff000d02 claims prop ff000d0f by "
                      "touch 3: granted, epoch 1; 2 listener(s) told") == 1);
    REQUIRE(log.Count("prop ff000d03") == 0);
  }
  REQUIRE(p.PropLiveOwner(crate) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  for (const Networking::UserId user :
       std::vector<Networking::UserId>{ 0, 1 }) {
    const auto words = SentTo<PropOwnerMessage>(p, user);
    REQUIRE(words.size() == 1);
    REQUIRE(words[0].refr == kCrate);
    REQUIRE(words[0].player == kC2);
  }
  Leave(p);
}

TEST_CASE("A live prop's owner stays its listener where the grid would drop "
          "it: a body two squares from its owner keeps the owner subscribed "
          "and its samples taken, the owner's own walk across a square keeps "
          "it too, and the next grid pass after the body's rest, or after "
          "the owner's unload of the prop's cell, drops it",
          "[Prop][espm]")
{
  // physics rung 3 (thuum lab/.cache/drafts/physics/rung3-plan.md B5): a body
  // the third word throws can land more than a square from its caster,
  // outside the 3x3 squares around it that decide its listeners, and the grid
  // would then unsubscribe its owner (a DestroyActor to the game whose engine
  // flies it, and no word on it after); the keep is npc-handoff D6's for an
  // actor's simulator (MpObjectReference's ForceSubscriptionsUpdate), on both
  // sides of the pair. c1 hosts the cell, c2 casts
  auto& p = GetPartOne();
  auto w = Enter(p);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);
  Claim(p, 1, kBasket, 3);
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  p.Messages().clear();

  // the flight's samples carried the record two squares east, 8,200 units
  // from c2 (x 17200, grid square 4 against c2's 2, Tamriel's cell (4, 0)):
  // the record stands where they left it, as the tether case stands its
  // rock, since no clock runs here. That write's grid pass drops the host's
  // game and keeps the owner's
  const NiPoint3 far = kHere + NiPoint3{ 8200.f, 0.f, 0.f };
  w.prop.SetPos(far);
  REQUIRE(Listens(w.prop, c2));
  REQUIRE(DestroysTo(p, 1, w.prop) == 0);
  REQUIRE_FALSE(Listens(w.prop, c1));
  REQUIRE(DestroysTo(p, 0, w.prop) == 1);
  // c2's sample there is taken (a live owner's stream passes the listener
  // check wherever it lands; the keep is what keeps its game told)
  const NiPoint3 flying = far + NiPoint3{ 50.f, 0.f, 0.f };
  Sample(p, 1, kBasket, flying, false);
  REQUIRE(w.prop.GetPos() == flying);

  // c2's own walk across a square while the body flies: its grid pass keeps
  // the pair too, where it drops a resting prop two squares away (the claim
  // case above, c2 at kTwoEast)
  c2.SetPos(kHere + NiPoint3{ -4000.f, 0.f, 0.f });
  REQUIRE(Listens(w.prop, c2));
  REQUIRE(DestroysTo(p, 1, w.prop) == 0);

  // the rest: the host's again at epoch 2, and c2, a listener still, hears
  // its own rest's word; the next grid pass, the body's own, parts the pair
  p.Messages().clear();
  Sample(p, 1, kBasket, flying + NiPoint3{ 10.f, 0.f, 0.f }, true);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  REQUIRE(Listens(w.prop, c2));
  const auto rested = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(rested.size() == 1);
  REQUIRE(rested[0].player == 0);
  REQUIRE(rested[0].epoch == 2);
  w.prop.ForceSubscriptionsUpdate();
  REQUIRE_FALSE(Listens(w.prop, c2));
  REQUIRE(DestroysTo(p, 1, w.prop) == 1);

  // the keep ends at an unload's return as at the rest (D-PH2 A): the record
  // back beside the players (the server's own placement, which subscribes
  // c2 again), c2's fresh grant at epoch 3, the body two squares off again,
  // kept; c2's game reports the prop's cell unloaded, which returns its live
  // row there to the host, and the body's next grid pass parts the pair
  w.prop.SetPos(kHere);
  REQUIRE(Listens(w.prop, c2));
  Claim(p, 1, kBasket, 3);
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  REQUIRE(p.PropEpochOf(w.prop) == 3);
  w.prop.SetPos(far);
  REQUIRE(Listens(w.prop, c2));
  CellLoaded(p, 1, w.cell, false);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  w.prop.ForceSubscriptionsUpdate();
  REQUIRE_FALSE(Listens(w.prop, c2));
  Leave(p);
}

// physics rung 4 (thuum ADR-071; Eli's D-PH9 A, 2026-11-12): the loader
// attaches every movable static, whatever its class, which is each seat's
// read of the loaded 3D; the server holds the reference like any world
// object of its chunk
TEST_CASE("The server holds a movable static after its chunk loads, at its "
          "record's position",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  const NiPoint3 placed = PlacedAt(p, kSign);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSign) == nullptr);
  Player(p, 0, kC1, placed + NiPoint3{ 40.f, 0.f, 0.f });
  const auto& form = p.worldState.LookupFormByIdNoLoad(kSign);
  REQUIRE(form != nullptr);
  MpObjectReference* sign = form->AsObjectReference();
  REQUIRE(sign != nullptr);
  REQUIRE(sign->AsActor() == nullptr);
  REQUIRE(sign->GetBaseType() == "MSTT");
  REQUIRE(sign->GetBaseId() == kSignBase);
  REQUIRE(sign->GetPos() == placed);
  REQUIRE(Listens(*sign, p.worldState.GetFormAt<MpActor>(kC1)));
  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
}

// thuum tracker #120 (playtest 54, Eli: "Props jumped in riverwood. Items
// near the blacksmiths forge."): an iron ingot on Riverwood's forge bench,
// 0x17355, a REFR of IngotIron 0x5ace4 (a MISC) in Tamriel 0x3c, record
// flags 0, no enable parent, no override in Update.esm or the three DLCs
// (lab/esm.py ref on Skyrim.esm, 2026-11-17), whose position here is read
// from its record, never typed
constexpr uint32_t kForgeIngot = 0x17355;

// a game-file prop's record rides behind its create word only once it holds
// play (PoseRecorded, PartOne's onSubscribe): every engine loads the game
// file's placement itself, so a seat is sent no record to put an untouched
// item back at, and a moved one's record is the place its copy goes
TEST_CASE("A game-file prop's create word comes alone while its record is "
          "the game file's placement, and with its record at seq 0 behind "
          "it once the record holds a move or a turn",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  const NiPoint3 placed = PlacedAt(p, kForgeIngot);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kForgeIngot) == nullptr);
  // where in the messages to `user` the first create word and the first
  // record of the ingot stand, -1 for none, and how many records went
  const auto createAt = [&](Networking::UserId user) {
    int at = 0;
    for (auto& m : p.Messages()) {
      auto create = dynamic_cast<CreateActorMessage*>(m.message.get());
      if (m.userId == user && create && create->refrId == kForgeIngot) {
        return at;
      }
      ++at;
    }
    return -1;
  };
  const auto records = [&](Networking::UserId user) {
    std::vector<std::pair<int, PropTransformMessage>> out;
    int at = 0;
    for (auto& m : p.Messages()) {
      auto record = dynamic_cast<PropTransformMessage*>(m.message.get());
      if (m.userId == user && record && record->refr == kForgeIngot) {
        out.push_back({ at, *record });
      }
      ++at;
    }
    return out;
  };

  // c1's grid pass loads the ingot's chunk: its create word, no record
  Player(p, 0, kC1, placed + NiPoint3{ 40.f, 0.f, 0.f });
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kForgeIngot) != nullptr);
  auto& ingot = p.worldState.GetFormAt<MpObjectReference>(kForgeIngot);
  REQUIRE(ingot.GetPos() == placed);
  REQUIRE_FALSE(ingot.PoseRecorded());
  REQUIRE(Listens(ingot, p.worldState.GetFormAt<MpActor>(kC1)));
  REQUIRE(createAt(0) >= 0);
  REQUIRE(records(0).empty());

  // a recorded rest 12 units east (the record a seat's stream leaves), then
  // c2's grid pass: its create word and the record behind it, at seq 0 and
  // the moved place; c1, a listener all along, is sent no record by the move
  const NiPoint3 moved = placed + NiPoint3{ 12.f, 0.f, 0.f };
  ingot.SetPos(moved);
  REQUIRE(ingot.PoseRecorded());
  p.Messages().clear();
  Player(p, 1, kC2, placed + NiPoint3{ 60.f, 0.f, 0.f });
  REQUIRE(Listens(ingot, p.worldState.GetFormAt<MpActor>(kC2)));
  const auto toC2 = records(1);
  REQUIRE(toC2.size() == 1);
  REQUIRE(toC2[0].second.seq == 0);
  REQUIRE(toC2[0].second.pos[0] == moved.x);
  REQUIRE(toC2[0].second.pos[1] == moved.y);
  REQUIRE(toC2[0].second.pos[2] == moved.z);
  REQUIRE(createAt(1) >= 0);
  REQUIRE(createAt(1) < toC2[0].first);
  REQUIRE(records(0).empty());

  // back at the placement it holds no play; a whole turn more is the same
  // angle, and five degrees of turn alone are play
  ingot.SetPos(placed);
  REQUIRE_FALSE(ingot.PoseRecorded());
  const NiPoint3 angle = ingot.GetAngle();
  ingot.SetAngle(angle + NiPoint3{ 0.f, 0.f, 360.f });
  REQUIRE_FALSE(ingot.PoseRecorded());
  ingot.SetAngle(angle + NiPoint3{ 0.f, 0.f, 5.f });
  REQUIRE(ingot.PoseRecorded());

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// physics rung 4 (D-PH9 A: the server admits a world body by any touch, as
// an item; the impulse-only gate of D-S3 point 4 retires, and a contact or
// a grab on a world body was E_PROP_KIND before)
TEST_CASE("A world body is claimed by a contact like an item, the word to "
          "both seats, and a resting trap rock by a grab",
          "[Prop][Traps][espm]")
{
  {
    auto& p = GetPartOne();
    auto w = EnterBySign(p);
    REQUIRE(p.PropOwnerOf(w.prop) == kC1);
    REQUIRE(p.PropEpochOf(w.prop) == 0);
    {
      LogCapture log;
      Claim(p, 1, kSign, 0);
      REQUIRE(log.Count("[verb:prop-physics] ff000d02 claims prop 5161a by "
                        "touch 0: granted, epoch 1; 2 listener(s) told") == 1);
      REQUIRE(log.Count("E_PROP_") == 0);
    }
    REQUIRE(p.PropOwnerOf(w.prop) == kC2);
    REQUIRE(p.PropLiveOwner(w.prop) == kC2);
    REQUIRE(p.PropEpochOf(w.prop) == 1);
    for (const Networking::UserId user :
         std::vector<Networking::UserId>{ 0, 1 }) {
      const auto words = SentTo<PropOwnerMessage>(p, user);
      REQUIRE(words.size() == 1);
      REQUIRE(words[0].refr == kSign);
      REQUIRE(words[0].player == kC2);
      REQUIRE(words[0].epoch == 1);
    }
    Leave(p);
  }
  {
    auto& p = GetPartOne();
    Player(p, 0, kC1, kByTheRocks, kEmbershard);
    Player(p, 1, kC2, kByTheRocks + NiPoint3{ 20.f, 0.f, 0.f }, kEmbershard);
    CellLoaded(p, 0, kEmbershard);
    REQUIRE(p.GetCellHost(kEmbershard).player == kC1);
    auto& rock = p.worldState.GetFormAt<MpObjectReference>(kRock);
    REQUIRE(rock.GetBaseType() == "ACTI");
    REQUIRE(p.PropEpochOf(rock) == 0);
    p.Messages().clear();
    Claim(p, 1, kRock, 1);
    REQUIRE(p.PropOwnerOf(rock) == kC2);
    REQUIRE(p.PropLiveOwner(rock) == kC2);
    REQUIRE(p.PropEpochOf(rock) == 1);
    REQUIRE(SentTo<PropOwnerMessage>(p, 0).size() == 1);
    REQUIRE(SentTo<PropOwnerMessage>(p, 1).size() == 1);
    REQUIRE(SentTo<PropOwnerMessage>(p, 1)[0].player == kC2);
    Leave(p);
  }
}

// physics rung 4 (D-PH10 A; DEF-4c): a world body's sample carries the poses
// of its parts as bones, which the server relays with the sample's own
// bytes and never records: the record takes the sample's pos and rot, the
// root's (the sign's anchor), and its answer to a refused sample carries
// none. The relay's bones on the wire are T2's to read (difftest's
// prop-world-body session): the legacy path here relays the raw bytes
TEST_CASE("A world body's sample carries its parts: taken and relayed with "
          "its bones, the record keeping pos and rot alone, and its rest "
          "returning the row with player 0",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = EnterBySign(p);
  const NiPoint3 placed = w.prop.GetPos();
  Claim(p, 1, kSign, 0);
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  p.Messages().clear();

  // c2's awake sample with the sign's seven parts: taken, relayed to c1
  // unreliable while awake
  const NiPoint3 swung = placed + NiPoint3{ 0.f, 0.f, -2.f };
  {
    LogCapture log;
    p.GetActionListener().OnPropTransform(
      Raw(1), SampleWithBones(p, kSign, swung, false, 7));
    REQUIRE(log.Count("E_PROP_") == 0);
  }
  REQUIRE(w.prop.GetPos() == swung);
  REQUIRE(w.prop.GetAngle() == NiPoint3{ 0.f, 0.f, 72.f });
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 0 });
  REQUIRE(RelaysTo(p, 1).empty());

  // the record holds no bones: c1's sample (not the owner's) is answered
  // with the word and the record, which is the sample's pos with none
  p.Messages().clear();
  Sample(p, 0, kSign, placed + NiPoint3{ 0.f, 0.f, -9.f }, false);
  REQUIRE(w.prop.GetPos() == swung);
  const auto record = SentTo<PropTransformMessage>(p, 0);
  REQUIRE(record.size() == 1);
  REQUIRE(record[0].seq == 0);
  REQUIRE(record[0].pos == std::array<float, 3>{ swung.x, swung.y, swung.z });
  REQUIRE(record[0].bones.empty());
  REQUIRE(RelaysTo(p, 1).empty());

  // c2's asleep sample with its parts: relayed reliable, and the row is the
  // host's again at epoch 2, both seats told with player 0 (A4)
  p.Messages().clear();
  const NiPoint3 rested = placed + NiPoint3{ 0.f, 0.f, -1.f };
  p.GetActionListener().OnPropTransform(
    Raw(1), SampleWithBones(p, kSign, rested, true, 7));
  REQUIRE(w.prop.GetPos() == rested);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 1 });
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropOwnerOf(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  for (const Networking::UserId user :
       std::vector<Networking::UserId>{ 0, 1 }) {
    const auto words = SentTo<PropOwnerMessage>(p, user);
    REQUIRE(words.size() == 1);
    REQUIRE(words[0].player == 0);
    REQUIRE(words[0].epoch == 2);
  }
  Leave(p);
}

// physics rung 4 (DEF-4h): the Rust edge refuses a non-finite bone and more
// bones than cap::PROP_BONES at decode; the legacy path's JSON (1e999 reads
// as infinity) is held to both in the core, before any arithmetic on them
TEST_CASE("A bone that is not a number, or more than 64 bones, is refused "
          "E_PROP_RANGE: the record unmoved, nothing relayed, nobody told; "
          "64 bones are taken",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = EnterBySign(p);
  const NiPoint3 placed = w.prop.GetPos();
  const NiPoint3 kicked = placed + NiPoint3{ 0.f, 0.f, -3.f };
  {
    LogCapture log;
    // c2: 65 bones, each finite and within the reach
    p.GetActionListener().OnPropTransform(
      Raw(1), SampleWithBones(p, kSign, kicked, false, 65));
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_RANGE: ff000d02 moves "
                      "prop 5161a; more bones than the wire carries") == 1);
    // c1: seven bones, the third's position infinite (the line), then a
    // quaternion lane not a number (the seat's second refusal: no line)
    auto infinite = SampleWithBones(p, kSign, kicked, false, 7);
    infinite.bones[2].pos[0] = std::numeric_limits<float>::infinity();
    p.GetActionListener().OnPropTransform(Raw(0), infinite);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_RANGE: ff000d01 moves "
                      "prop 5161a; a transform that is not a number") == 1);
    auto nan = SampleWithBones(p, kSign, kicked, false, 7);
    nan.bones[6].quat[1] = std::numeric_limits<float>::quiet_NaN();
    p.GetActionListener().OnPropTransform(Raw(0), nan);
    REQUIRE(log.Count("E_PROP_RANGE") == 2);
    REQUIRE(log.Count("woken by its host") == 0);
  }
  REQUIRE(w.prop.GetPos() == placed);
  REQUIRE(p.PropEpochOf(w.prop) == 0);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(RelaysTo(p, 0).empty());
  REQUIRE(RelaysTo(p, 1).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  REQUIRE(SentTo<PropTransformMessage>(p, 0).empty());
  REQUIRE(SentTo<PropTransformMessage>(p, 1).empty());

  // the cap itself is taken: the host's sample with 64 finite bones wakes
  // the resting sign by id (A7)
  p.GetActionListener().OnPropTransform(
    Raw(0), SampleWithBones(p, kSign, kicked, false, 64));
  REQUIRE(w.prop.GetPos() == kicked);
  REQUIRE(p.PropLiveOwner(w.prop) == kC1);
  REQUIRE(p.PropEpochOf(w.prop) == 1);
  REQUIRE(RelaysTo(p, 1) == std::vector<int>{ 0 });
  Leave(p);
}

// physics rung 4 (DEF-4g; risk H-S6): a clamped rest is re-encoded for the
// other listeners at the record's new point, and a world body's parts move
// with it as one rigid shift, so each bone stands where it stood from the
// root and the outbound validator's 512 (wire-validate validate_server)
// holds; the step budget is the container case's: the epoch's first sample
// leaves one step, 600 units, and the tether (2048 from the record's file
// position) admits the far rest
TEST_CASE("A clamped rest of a world body moves its bones with the record: "
          "the relay's position at the rest and each bone shifted by the "
          "same delta",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto w = EnterBySign(p);
  const NiPoint3 placed = w.prop.GetPos();
  Claim(p, 1, kSign, 0);
  REQUIRE(p.PropLiveOwner(w.prop) == kC2);
  const NiPoint3 near = placed + NiPoint3{ 50.f, 0.f, 0.f };
  Sample(p, 1, kSign, near, false);
  REQUIRE(w.prop.GetPos() == near);
  p.Messages().clear();

  // the rest 1,500 units past the record and 1,550 from the file position
  const NiPoint3 far = placed + NiPoint3{ 1550.f, 0.f, 0.f };
  const auto sent = SampleWithBones(p, kSign, far, true, 7);
  p.GetActionListener().OnPropTransform(Raw(1), sent);
  const NiPoint3 rest = near + NiPoint3{ 600.f, 0.f, 0.f };
  REQUIRE(std::abs(w.prop.GetPos().x - rest.x) < 1.f);
  REQUIRE(p.PropLiveOwner(w.prop) == 0);
  REQUIRE(p.PropEpochOf(w.prop) == 2);
  // c1 hears the rest re-encoded, never the sender's bytes
  REQUIRE(RelaysTo(p, 0).empty());
  const auto relay = SentTo<PropTransformMessage>(p, 0);
  REQUIRE(relay.size() == 1);
  REQUIRE(relay[0].asleep);
  REQUIRE(std::abs(relay[0].pos[0] - rest.x) < 1.f);
  REQUIRE(std::abs(relay[0].pos[1] - rest.y) < 1.f);
  REQUIRE(std::abs(relay[0].pos[2] - rest.z) < 1.f);
  REQUIRE(relay[0].bones.size() == sent.bones.size());
  const NiPoint3 shift{ relay[0].pos[0] - far.x, relay[0].pos[1] - far.y,
                        relay[0].pos[2] - far.z };
  REQUIRE(std::abs(shift.x - (rest.x - far.x)) < 1.f);
  for (size_t i = 0; i < sent.bones.size(); ++i) {
    for (size_t k = 0; k < 3; ++k) {
      REQUIRE(std::abs(relay[0].bones[i].pos[k] -
                       (sent.bones[i].pos[k] + shift[k])) < 0.01f);
    }
    REQUIRE(relay[0].bones[i].quat == sent.bones[i].quat);
    const NiPoint3 bone{ relay[0].bones[i].pos[0], relay[0].bones[i].pos[1],
                         relay[0].bones[i].pos[2] };
    REQUIRE((bone - rest).Length() < 512.f);
  }
  // and the sender the record, whose answer carries no bones
  const auto record = SentTo<PropTransformMessage>(p, 1);
  REQUIRE(record.size() == 1);
  REQUIRE(record[0].seq == 0);
  REQUIRE(record[0].bones.empty());
  Leave(p);
}

// physics rung 4 (DEF-4e): a world body is a game-file reference, held to its
// file position by the tether; a server-made one (a spell's placed wall,
// SpellPlacedTest's MSTT base) has none and is no prop at all, by any touch
TEST_CASE("A server-made world object is no prop: a contact and an impulse "
          "on a server-made movable static are refused E_PROP_KIND with no "
          "word, and its sample moves nothing",
          "[Prop][espm]")
{
  // Skyrim.esm's MSTT BFXSmashedBarrelWithOil (SpellPlacedTest.cpp:31-33),
  // its type read from the record here
  constexpr uint32_t kSmashedBarrel = 0x000eee0d;
  constexpr uint32_t kPlacedWall = 0xff000d17;
  auto& p = GetPartOne();
  const auto base = p.GetEspm().GetBrowser().LookupById(kSmashedBarrel);
  REQUIRE(base.rec);
  REQUIRE(base.rec->GetType() == "MSTT");
  Enter(p);
  const NiPoint3 wallAt = kHere + NiPoint3{ 30.f, 0.f, 0.f };
  {
    auto wall = std::make_unique<MpObjectReference>(
      LocationalData{ wallAt, { 0.f, 0.f, 0.f }, FormDesc::Tamriel() },
      p.CreateFormCallbacks(), kSmashedBarrel, "MSTT");
    p.worldState.AddForm(std::move(wall), kPlacedWall);
  }
  auto& wall = p.worldState.GetFormAt<MpObjectReference>(kPlacedWall);
  wall.ForceSubscriptionsUpdate();
  REQUIRE(Listens(wall, p.worldState.GetFormAt<MpActor>(kC1)));
  REQUIRE(Listens(wall, p.worldState.GetFormAt<MpActor>(kC2)));
  p.Messages().clear();
  {
    LogCapture log;
    Claim(p, 0, kPlacedWall, 0);
    Claim(p, 1, kPlacedWall, 3);
    REQUIRE(
      log.Count("[verb:prop-physics] E_PROP_KIND: ff000d01 claims "
                "prop ff000d17 by touch 0; neither an item, a world body "
                "nor an NPC's or a player's body") == 1);
    REQUIRE(
      log.Count("[verb:prop-physics] E_PROP_KIND: ff000d02 claims "
                "prop ff000d17 by touch 3; neither an item, a world body "
                "nor an NPC's or a player's body") == 1);
  }
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  REQUIRE(p.PropEpochOf(wall) == 0);
  // the host's sample of it: refused by its kind too (the seat's second
  // refusal inside 10 s, so its effect is read)
  Sample(p, 0, kPlacedWall, wallAt + NiPoint3{ 0.f, 0.f, -20.f }, false);
  REQUIRE(wall.GetPos() == wallAt);
  REQUIRE(RelaysTo(p, 1).empty());
  REQUIRE(p.PropEpochOf(wall) == 0);
  REQUIRE(p.PropLiveOwner(wall) == 0);
  Leave(p);
}

// physics rung 4 (DEF-4f: D-PH9 A gives a STAT with a dynamic model no row):
// a STAT the server holds is one it attached by its record (thuum ADR-068
// rung 4's AttachForRecord, AttachByRecordTest's Bleak Falls Temple XMarker
// 0x108a5c, a REFR of the STAT 0x3b at (-598, -4954, -3312)); it is no world
// body for a claim, a sample or a destruction report
TEST_CASE("A STAT attached by its record is no world body: a claim and a "
          "sample are refused E_PROP_KIND and a destruction report "
          "E_DESTRUCTION_KIND",
          "[Prop][Traps][espm]")
{
  constexpr uint32_t kTemple = 0x371de;
  constexpr uint32_t kMarker = 0x108a5c;
  auto& p = GetPartOne();
  REQUIRE(p.worldState.AttachForRecord(kMarker));
  auto& marker = p.worldState.GetFormAt<MpObjectReference>(kMarker);
  REQUIRE(marker.GetBaseType() == "STAT");
  const NiPoint3 markerAt = marker.GetPos();
  Player(p, 0, kC1, markerAt + NiPoint3{ 40.f, 0.f, 0.f }, kTemple);
  Player(p, 1, kC2, markerAt + NiPoint3{ 60.f, 0.f, 0.f }, kTemple);
  p.Messages().clear();
  {
    LogCapture log;
    Claim(p, 0, kMarker, 0);
    Sample(p, 1, kMarker, markerAt + NiPoint3{ 0.f, 0.f, -20.f }, false);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_KIND: ff000d01 claims "
                      "prop 108a5c by touch 0; neither an item, a world body "
                      "nor an NPC's or a player's body") == 1);
    REQUIRE(
      log.Count("[verb:prop-physics] E_PROP_KIND: ff000d02 moves prop "
                "108a5c; neither an item, a world body nor an NPC's or a "
                "player's body") == 1);
    DestructionStageMessage report;
    report.refr = kMarker;
    report.stage = 1;
    report.destroyed = false;
    report.served = false;
    p.GetActionListener().OnDestructionStage(Raw(0), report);
    REQUIRE(log.Count("[verb:traps] E_DESTRUCTION_KIND: ff000d01 reports "
                      "108a5c at stage 1, destroyed false: not a trap or "
                      "destructible world body") == 1);
  }
  REQUIRE(marker.GetPos() == markerAt);
  REQUIRE(p.PropEpochOf(marker) == 0);
  REQUIRE(!marker.GetDestruction().has_value());
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).empty());
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  Leave(p);
}

// physics rung 5 (ADR-071 decision 5; thuum tracker #175, playtest 56: "there
// is ZERO syncing currently. nothing at all."): a dead NPC's body is a prop of
// the Body kind. A touch claims it (a drag's grab here), the owner's samples
// move the record and reach the other seat with no tether, and its rest
// returns it to the cell's host with the word, as any prop's; a knocked
// living actor is a live body, claimed and returned alike (the live-body kind,
// Eli's second look at #175). ActorDeathTest's fixture: the forest's
// leveled predator 0x85547 (NpcFateTest's ids), loaded by the arrival's fate
// pass
TEST_CASE("A dead NPC's body is a prop: a grab claims it, its samples move "
          "the record and reach the other seat, and its rest returns it to "
          "the host; a knocked living NPC is a live body, claimed and "
          "returned alike",
          "[Prop][espm]")
{
  constexpr uint32_t kPredator = 0x85547;
  const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  Player(p, 0, kC1, kForest);
  Player(p, 1, kC2, kForest + NiPoint3{ 20.f, 0.f, 0.f });
  p.TickCellHosts(NowMs());
  const auto& form = p.worldState.LookupFormByIdNoLoad(kPredator);
  REQUIRE(form != nullptr);
  MpActor* body = form->AsActor();
  REQUIRE(body != nullptr);
  if (body->IsDead()) {
    body->Respawn(false);
  }
  p.Messages().clear();
  {
    // alive and knocked (thuum tracker #175, the live-body kind): the seat
    // whose cause moved it claims it, and its asleep sample at the get-up
    // returns it to the host
    LogCapture log;
    Claim(p, 1, kPredator, 1);
    REQUIRE(log.Count("[verb:prop-physics] ff000d02 claims prop 85547 by "
                      "touch 1: granted") == 1);
  }
  REQUIRE(p.PropLiveOwner(*body) == kC2);
  Sample(p, 1, kPredator, body->GetPos(), true);
  REQUIRE(p.PropLiveOwner(*body) == 0);
  p.Messages().clear();
  body->Kill(nullptr);
  REQUIRE(body->IsDead());
  const NiPoint3 diedAt = body->GetPos();
  p.Messages().clear();
  {
    LogCapture log;
    Claim(p, 1, kPredator, 1);
    REQUIRE(log.Count("[verb:prop-physics] ff000d02 claims prop 85547 by "
                      "touch 1: granted") == 1);
  }
  const auto granted = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(granted.size() == 1);
  REQUIRE(granted[0].refr == kPredator);
  REQUIRE(granted[0].player == kC2);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).size() == 1);
  REQUIRE(p.PropLiveOwner(*body) == kC2);
  // the drag: the holder's samples move the record and reach the other seat
  p.Messages().clear();
  auto drag =
    SampleOf(p, kPredator, diedAt + NiPoint3{ 30.f, 0.f, 0.f }, false);
  drag.held = true;
  p.GetActionListener().OnPropTransform(Raw(1), drag);
  REQUIRE((body->GetPos() - (diedAt + NiPoint3{ 30.f, 0.f, 0.f })).Length() <
          0.01f);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 0 });
  REQUIRE(RelaysTo(p, 1).empty());
  // the other seat's sample of a body it does not hold is refused
  {
    LogCapture log;
    Sample(p, 0, kPredator, diedAt + NiPoint3{ 0.f, 30.f, 0.f }, false);
    REQUIRE(log.Count("[verb:prop-physics] E_PROP_NOT_OWNER: ff000d01 moves "
                      "prop 85547") == 1);
  }
  REQUIRE((body->GetPos() - (diedAt + NiPoint3{ 30.f, 0.f, 0.f })).Length() <
          0.01f);
  // the release and the rest: the record keeps it, the host's again by word
  p.Messages().clear();
  Sample(p, 1, kPredator, diedAt + NiPoint3{ 40.f, 0.f, 0.f }, true);
  REQUIRE((body->GetPos() - (diedAt + NiPoint3{ 40.f, 0.f, 0.f })).Length() <
          0.01f);
  REQUIRE(RelaysTo(p, 0) == std::vector<int>{ 1 });
  const auto returned = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(returned.size() == 1);
  REQUIRE(returned[0].player == 0);
  REQUIRE(p.PropLiveOwner(*body) == 0);
  body->Respawn(false);
  p.worldState.fateRespawnDue.erase(kPredator);
  Leave(p);
}

TEST_CASE("A player's knocked body is its own game's alone: its own claim is "
          "granted and told to the other seat, another seat's refused, and "
          "its asleep sample at the get-up returns it",
          "[Prop][espm]")
{
  const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };
  auto& p = GetPartOne();
  Player(p, 0, kC1, kForest);
  Player(p, 1, kC2, kForest + NiPoint3{ 20.f, 0.f, 0.f });
  p.TickCellHosts(NowMs());
  const auto& form = p.worldState.LookupFormByIdNoLoad(kC1);
  REQUIRE(form != nullptr);
  MpActor* c1 = form->AsActor();
  REQUIRE(c1 != nullptr);
  p.Messages().clear();
  MpActor* c2 = p.worldState.LookupFormByIdNoLoad(kC2)->AsActor();
  REQUIRE(c2 != nullptr);
  // the shouter's game claims the player it threw: refused (the refusal's
  // line is said once in 10 s per seat, so the state reads it)
  Claim(p, 1, kC1, 3);
  REQUIRE(p.PropLiveOwner(*c1) == 0);
  REQUIRE(SentTo<PropOwnerMessage>(p, 1).empty());
  // and neither seat's sample moves the other player's body, the cell's host
  // among them (whichever seat hosts, one of the two is its sample)
  const NiPoint3 c1At = c1->GetPos();
  const NiPoint3 c2At = c2->GetPos();
  Sample(p, 1, kC1, c1At + NiPoint3{ 50.f, 0.f, 0.f }, false);
  Sample(p, 0, kC2, c2At + NiPoint3{ 50.f, 0.f, 0.f }, false);
  REQUIRE(c1->GetPos() == c1At);
  REQUIRE(c2->GetPos() == c2At);
  REQUIRE(p.PropLiveOwner(*c1) == 0);
  REQUIRE(p.PropLiveOwner(*c2) == 0);
  {
    LogCapture log;
    Claim(p, 0, kC1, 3);
    REQUIRE(log.Count("[verb:prop-physics] ff000d01 claims prop ff000d01 by "
                      "touch 3: granted") == 1);
  }
  REQUIRE(p.PropLiveOwner(*c1) == kC1);
  const auto words = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].refr == kC1);
  REQUIRE(words[0].player == kC1);
  Sample(p, 0, kC1, c1->GetPos(), true);
  REQUIRE(p.PropLiveOwner(*c1) == 0);
  Leave(p);
}

// thuum tracker #179 (ADR-063's 2026-11-19 amendment; Eli: "yeah go with
// it"): a Vampiric Grip on another player is a hand, the engine's grab, so
// the gripper's touch 1 makes the gripped player's body the gripper's for
// the hold, and the gripped player's own game follows that stream; any other
// touch of another player's body stays refused (its own game simulates its
// knock, #178)
TEST_CASE("Another player's grip holds a player's body: its grab is granted "
          "and told to the gripped player's own game, its samples move the "
          "body and reach that game, another touch stays refused, and the "
          "gripper's rest returns the body",
          "[Prop][espm]")
{
  const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };
  auto& p = GetPartOne();
  Player(p, 0, kC1, kForest);
  Player(p, 1, kC2, kForest + NiPoint3{ 20.f, 0.f, 0.f });
  p.TickCellHosts(NowMs());
  MpActor* c1 = p.worldState.LookupFormByIdNoLoad(kC1)->AsActor();
  REQUIRE(c1 != nullptr);
  p.Messages().clear();
  // c2's impulse on c1 (a shout's): refused, as before
  Claim(p, 1, kC1, 3);
  REQUIRE(p.PropLiveOwner(*c1) == 0);
  // c2's grip of c1: granted, and the word reaches c1's own game
  {
    LogCapture log;
    Claim(p, 1, kC1, 1);
    REQUIRE(log.Count("[verb:prop-physics] ff000d02 claims prop ff000d01 by "
                      "touch 1: granted") == 1);
  }
  REQUIRE(p.PropLiveOwner(*c1) == kC2);
  const auto words = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].refr == kC1);
  REQUIRE(words[0].player == kC2);
  // the gripper's sample moves c1's body and reaches c1's own game
  p.Messages().clear();
  const NiPoint3 lifted = c1->GetPos() + NiPoint3{ 0.f, 30.f, 20.f };
  Sample(p, 1, kC1, lifted, false);
  REQUIRE(c1->GetPos() == lifted);
  REQUIRE(RelaysTo(p, 0).size() == 1);
  // c1's own game claims its body while the gripper streams it: refused
  Claim(p, 0, kC1, 3);
  REQUIRE(p.PropLiveOwner(*c1) == kC2);
  // the gripper's rest (its copy's get-up) returns the body: player 0
  p.Messages().clear();
  Sample(p, 1, kC1, lifted, true);
  REQUIRE(p.PropLiveOwner(*c1) == 0);
  const auto back = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(back.size() == 1);
  REQUIRE(back[0].player == 0);
  // gripped again, the gripped player leaves mid-hold: its own body's row
  // ends with the word to the gripper (the review of 48959a563, finding 4:
  // the row stayed the gripper's and refused its own knock after a return)
  Claim(p, 1, kC1, 1);
  REQUIRE(p.PropLiveOwner(*c1) == kC2);
  p.Messages().clear();
  DoDisconnect(p, 0);
  REQUIRE(p.PropLiveOwner(*c1) == 0);
  const auto ended = SentTo<PropOwnerMessage>(p, 1);
  REQUIRE(ended.size() == 1);
  REQUIRE(ended[0].refr == kC1);
  REQUIRE(ended[0].player == 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// Dawnguard.esm: the Vampire Lord's grip, DLC1nVampireBloodyGripSpell
// 0x0200E7D2, is a concentration spell whose cloak effect
// DLC1nVampireBloodyGripCloakEffect 0x0200E7D7 reaches DLC1VampireGripDmg
// 0x0200E7DA, an Absorb Health of 5 a second whose effect 0x0200E7E8 holds
// one condition, GetActorValue(Grabbed) == 1: the engine's mark on the actor
// a grab holds, set only in the gripper's game
namespace {
constexpr uint32_t kGripSpell = 0x0200e7d2;
constexpr uint32_t kGripDmg = 0x0200e7da;

// two players in the forest, `gripped` with a recorded base health of 250
// and the gripper with the grip in its right hand, cast
MpActor& GripPair(PartOne& p, uint32_t gripped, uint32_t gripper)
{
  const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };
  auto& body = Player(p, 0, gripped, kForest);
  auto& hands = Player(p, 1, gripper, kForest + NiPoint3{ 20.f, 0.f, 0.f });
  p.TickCellHosts(NowMs());
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(body.SetActorValueRecord(record));
  Equipment eq;
  eq.rightSpell = kGripSpell;
  hands.SetEquipment(eq);
  DoMessage(
    p, 1,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", 0 },
                        { "spell", kGripSpell },
                        { "isDualCasting", false },
                        { "interruptCast", false },
                        { "castingSource", 1 },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "hasLaunch", false },
                        { "originX", 0.f },
                        { "originY", 0.f },
                        { "originZ", 0.f },
                        { "launchAngleX", 0.f },
                        { "launchAngleZ", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
  return body;
}

// the gripper's game's report of the grip's drain on `gripped`
void GripDrains(PartOne& p, uint32_t gripped)
{
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = gripped;
  hit.data.source = kGripDmg;
  p.GetActionListener().OnHit(Raw(1), hit);
}

void GripLeave(PartOne& p, uint32_t gripped, uint32_t gripper)
{
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
  p.DestroyActor(gripped);
  p.DestroyActor(gripper);
}
}

TEST_CASE("A grip's drain takes health from the body its grab holds",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto& body = GripPair(p, kC1, kC2);
  Claim(p, 1, kC1, 1);
  REQUIRE(p.PropLiveOwner(body) == kC2);
  GripDrains(p, kC1);
  // the first report counts one cadence, half a second: 2.5 of 250
  REQUIRE(body.GetChangeForm().actorValues.healthPercentage ==
          Catch::Approx(0.99f));
  GripLeave(p, kC1, kC2);
}

TEST_CASE("A grip's drain takes nothing from a body its grab let go",
          "[Prop][espm]")
{
  auto& p = GetPartOne();
  auto& body = GripPair(p, kC3, kC4);
  Claim(p, 1, kC3, 1);
  REQUIRE(p.PropLiveOwner(body) == kC4);
  // the gripper's rest (its copy's get-up) returns the body
  Sample(p, 1, kC3, body.GetPos(), true);
  REQUIRE(p.PropLiveOwner(body) == 0);
  GripDrains(p, kC3);
  REQUIRE(body.GetChangeForm().actorValues.healthPercentage ==
          Catch::Approx(1.f));
  GripLeave(p, kC3, kC4);
}

// A player's shout throws another player the grip's way: the shouter's own
// game throws its copy, so its impulse (touch 3) on the player its shout hit
// makes the body the shouter's for the flight, once the server took that hit
// (PartOne::ClaimedHit); the thrown player's own game follows that stream,
// and the shouter's rest returns the body. An impulse with no hit taken stays
// refused
TEST_CASE("A player's shout throws another player's body: the shouter's "
          "impulse is granted once its hit on that player is taken, told to "
          "the thrown player's own game, its samples move the body, and its "
          "rest returns it",
          "[Prop][espm]")
{
  // VoiceUnrelentingForce1 (Skyrim.esm), the shout's first word's spell
  constexpr uint32_t kFus = 0x00013e09;
  const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };
  auto& p = GetPartOne();
  Player(p, 0, kC1, kForest);
  Player(p, 1, kC2, kForest + NiPoint3{ 200.f, 0.f, 0.f });
  p.TickCellHosts(NowMs());
  MpActor* c1 = p.worldState.LookupFormByIdNoLoad(kC1)->AsActor();
  REQUIRE(c1 != nullptr);
  p.Messages().clear();
  // c2's impulse on c1 with no hit of its shout's taken: refused
  Claim(p, 1, kC1, 3);
  REQUIRE(p.PropLiveOwner(*c1) == 0);
  REQUIRE(SentTo<PropOwnerMessage>(p, 0).empty());
  // c2's shout hits c1, the server takes the hit: c2's impulse is granted
  p.StartCast(kC2, kFus, 2, kForest + NiPoint3{ 200.f, 0.f, 0.f });
  REQUIRE(p.ClaimCast(kC2, kFus, kC1, c1->GetPos()).check ==
          PartOne::CastCheck::Claimed);
  {
    LogCapture log;
    Claim(p, 1, kC1, 3);
    REQUIRE(log.Count("[verb:prop-physics] ff000d02 claims prop ff000d01 by "
                      "touch 3: granted") == 1);
  }
  REQUIRE(p.PropLiveOwner(*c1) == kC2);
  const auto words = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].refr == kC1);
  REQUIRE(words[0].player == kC2);
  // the shouter's sample moves c1's body and reaches c1's own game
  p.Messages().clear();
  const NiPoint3 thrown = c1->GetPos() + NiPoint3{ -60.f, 0.f, 25.f };
  Sample(p, 1, kC1, thrown, false);
  REQUIRE(c1->GetPos() == thrown);
  REQUIRE(RelaysTo(p, 0).size() == 1);
  // the shouter's rest returns the body: player 0, which c1's own game reads
  // as its own
  p.Messages().clear();
  Sample(p, 1, kC1, thrown, true);
  REQUIRE(p.PropLiveOwner(*c1) == 0);
  const auto back = SentTo<PropOwnerMessage>(p, 0);
  REQUIRE(back.size() == 1);
  REQUIRE(back[0].player == 0);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}
