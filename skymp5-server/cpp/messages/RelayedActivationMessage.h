#pragma once
#include "../server_guest_lib/Inventory.h"
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <type_traits>

// thuum docs/verbs/traps.md (M4): a linked activation the cell's host reported
// (a pressure plate firing its spike wall, a lever opening its portcullis),
// told to the cell's other games so each engine plays it with the caster the
// host's did (ActionListener::OnLinkedActivation). An actor target (ADR-068)
// is never told to the cell's other games: it goes to the one other seat
// that simulates the actor, or nowhere. Server to client,
// reliable. The contract is skymp-wire's (wire-schema skymp.rs,
// RelayedActivation, MsgType 70, wire id 78).
// Since SCHEMA_VERSION 40 (thuum D-C2): with `entry` the relay is a trigger
// entry a guest's game reported (ActionListener::OnTriggerEntry), sent to
// the cell's host alone, whose engine sends that event to the target's
// scripts with the caster as akActionRef instead of activating.
struct RelayedActivationMessage : public MessageBase<RelayedActivationMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::RelayedActivation)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("target", target)
      .Serialize("caster", caster)
      .Serialize("entry", entry)
      .Serialize("presserInventory", presserInventory)
      .Serialize("event", event)
      .Serialize("source", source)
      .Serialize("projectile", projectile)
      .Serialize("count", count)
      .Serialize("flags", flags);
  }

  // RelayedActivation's script events (wire-schema SCRIPT_EVENT_*)
  static constexpr uint32_t kEventActivate = 1;
  static constexpr uint32_t kEventHit = 2;
  static constexpr uint32_t kEventItemAdded = 3;
  static constexpr uint32_t kEventItemRemoved = 4;
  static constexpr uint32_t kEventEffectApply = 5;

  uint32_t target = 0; // the activated reference, never 0
  uint32_t caster = 0; // the reference that activated it, never 0
  // 1 OnTriggerEnter, 2 OnTriggerLeave (a u8 on the wire); absent on an
  // activation
  std::optional<uint32_t> entry;
  // since SCHEMA_VERSION 60 (thuum tracker #117, Eli's design B): on a
  // player's press of a scripted world object, sent to the cell's host
  // alone, the presser's inventory as the server records it, which the
  // host's acting scope answers the press's GetItemCount from; absent on an
  // entry, on a press by no player and to every other game
  std::optional<Inventory> presserInventory;
  // since SCHEMA_VERSION 69 (the one script channel's events): an act
  // another seat made on an object the receiver owns, sent to the object's
  // scripts alone (kEventActivate to kEventEffectApply, a u8 on the wire),
  // with the event's form (OnHit's source, an item event's base item, an
  // effect's apply's effect), OnHit's projectile and flags (wire-schema
  // HIT_*), an item event's count; each absent on an activation and an entry
  std::optional<uint32_t> event;
  std::optional<uint32_t> source;
  std::optional<uint32_t> projectile;
  std::optional<int32_t> count;
  std::optional<uint32_t> flags;
};
