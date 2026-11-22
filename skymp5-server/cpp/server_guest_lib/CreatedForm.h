#pragma once
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

// A form the game made at runtime, a brewed potion or poison or an
// enchantment made at the arcane enchanter, by its definition. Its id is the
// crafting engine's own (0xff000000 and up) and names nothing on any other
// game or after a relaunch, so the record and the wire carry the definition,
// and every game that receives a stack builds the same form through its own
// engine's creator. The contract is skymp-wire's (wire-schema skymp.rs,
// CreatedEffect and CreatedForm, SCHEMA_VERSION 61).

// One effect, in the engine's Effect units (CommonLibSSE-NG
// include/RE/E/Effect.h): its MGEF by form id, magnitude, area, duration and
// cost as the crafting menu handed them to the creator, final (the crafter's
// skill and perks applied)
struct CreatedEffect
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("mgef", mgef)
      .Serialize("magnitude", magnitude)
      .Serialize("area", area)
      .Serialize("duration", duration)
      .Serialize("cost", cost);
  }

  uint32_t mgef = 0;
  float magnitude = 0.f;
  uint32_t area = 0;
  uint32_t duration = 0;
  float cost = 0.f;

  auto ToTuple() const
  {
    return std::make_tuple(mgef, magnitude, area, duration, cost);
  }

  // What makes two effects one: assumes the engine's creators hand back an
  // existing form when its effects match in MGEF, magnitude, area and
  // duration, the cost not compared
  auto IdentityTuple() const
  {
    return std::make_tuple(mgef, magnitude, area, duration);
  }

  friend bool operator==(const CreatedEffect& lhs, const CreatedEffect& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const CreatedEffect& lhs, const CreatedEffect& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};

struct CreatedForm
{
  // The engine's four creators (BGSCreatedObjectManager's AddWeapon- and
  // AddArmorEnchantment, CommonLibSSE-NG include/RE/B/
  // BGSCreatedObjectManager.h, and its AddPotion and AddPoison)
  enum Kind : uint8_t
  {
    kWeaponEnchantment = 0,
    kArmorEnchantment = 1,
    kPotion = 2,
    kPoison = 3
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("kind", kind).Serialize("effects", effects);
  }

  uint8_t kind = 0;
  std::vector<CreatedEffect> effects;

  // Whether a stack of this form and a stack of `other` are one: the same
  // kind and each effect the same by IdentityTuple, in order, as the
  // engine's creator would hand back one form for both
  bool SameAs(const CreatedForm& other) const
  {
    if (kind != other.kind || effects.size() != other.effects.size()) {
      return false;
    }
    for (size_t i = 0; i < effects.size(); ++i) {
      if (effects[i].IdentityTuple() != other.effects[i].IdentityTuple()) {
        return false;
      }
    }
    return true;
  }

  auto ToTuple() const { return std::make_tuple(kind, effects); }

  friend bool operator==(const CreatedForm& lhs, const CreatedForm& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const CreatedForm& lhs, const CreatedForm& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};

// Both absent, or both present and the same form (CreatedForm::SameAs)
inline bool SameCreated(const std::optional<CreatedForm>& lhs,
                        const std::optional<CreatedForm>& rhs)
{
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }
  return !lhs || lhs->SameAs(*rhs);
}
