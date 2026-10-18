#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

struct ActorAnimationVariables
{
  std::vector<uint8_t> booleans;
  std::vector<uint8_t> floats;
  std::vector<uint8_t> integers;

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("booleans", booleans)
      .Serialize("floats", floats)
      .Serialize("integers", integers);
  }
};

struct SpellCastMsgData
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("caster", caster)
      .Serialize("target", target)
      .Serialize("spell", spell)
      .Serialize("isDualCasting", isDualCasting)
      .Serialize("interruptCast", interruptCast)
      .Serialize("castingSource", castingSource)
      .Serialize("aimAngle", aimAngle)
      .Serialize("aimHeading", aimHeading)
      .Serialize("hasLaunch", hasLaunch)
      .Serialize("originX", originX)
      .Serialize("originY", originY)
      .Serialize("originZ", originZ)
      .Serialize("launchAngleX", launchAngleX)
      .Serialize("launchAngleZ", launchAngleZ)
      .Serialize("actorAnimationVariables", actorAnimationVariables);
  }

  uint32_t caster = 0;
  uint32_t target = 0;
  uint32_t spell = 0;
  bool isDualCasting = false;
  bool interruptCast = false;
  int32_t castingSource = 0;
  float aimAngle = 0.f;
  float aimHeading = 0.f;
  // the caster's own launch as its engine made it (Projectile::Launch's
  // LaunchData): for a target-location spell, the point its game picked
  // along the caster's aim, which each neighbour's game launches the spell
  // at; the origin in engine units, pitch and heading in radians; without
  // it, all zero (thuum docs/verbs/spell-cast.md)
  bool hasLaunch = false;
  float originX = 0.f;
  float originY = 0.f;
  float originZ = 0.f;
  float launchAngleX = 0.f;
  float launchAngleZ = 0.f;

  ActorAnimationVariables actorAnimationVariables;
};

struct SpellCastMessage : public MessageBase<SpellCastMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::SpellCast)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("data", data);
  }

  SpellCastMsgData data;
};
