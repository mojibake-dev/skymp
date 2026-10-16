#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

struct PlayerBowShotMessage : public MessageBase<PlayerBowShotMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PlayerBowShot)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("weaponId", weaponId)
      .Serialize("ammoId", ammoId)
      .Serialize("power", power)
      .Serialize("isSunGazing", isSunGazing)
      .Serialize("aimAngle", aimAngle)
      .Serialize("aimHeading", aimHeading)
      .Serialize("hasLaunch", hasLaunch)
      .Serialize("originX", originX)
      .Serialize("originY", originY)
      .Serialize("originZ", originZ)
      .Serialize("launchAngleX", launchAngleX)
      .Serialize("launchAngleZ", launchAngleZ);
  }

  uint32_t weaponId = 0;
  uint32_t ammoId = 0;
  float power = 0.f;
  bool isSunGazing = false;
  // the aim when the arrow left, radians (thuum docs/verbs/marksman.md)
  float aimAngle = 0.f;
  float aimHeading = 0.f;
  // the shooter's own launch as its engine made it (Projectile::Launch's
  // LaunchData): the origin in engine units, pitch and heading in radians;
  // without it, all zero (thuum docs/verbs/marksman.md: the arrow's path
  // identical on both screens)
  bool hasLaunch = false;
  float originX = 0.f;
  float originY = 0.f;
  float originZ = 0.f;
  float launchAngleX = 0.f;
  float launchAngleZ = 0.f;
};
