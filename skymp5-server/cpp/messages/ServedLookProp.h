#pragma once
#include <cstdint>

// thuum #52 (wire-schema ServedLook, SCHEMA_VERSION 53; wire-rules
// actor_looks): one look an actor's simulator's script drew on it and has
// not stopped, served with its create word
struct ServedLookProp
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("keep", keep)
      .Serialize("form", form)
      .Serialize("seconds", seconds)
      .Serialize("facing", facing)
      .Serialize("seq", seq);
  }
  friend bool operator==(const ServedLookProp& lhs, const ServedLookProp& rhs)
  {
    return lhs.keep == rhs.keep && lhs.form == rhs.form &&
      lhs.seconds == rhs.seconds && lhs.facing == rhs.facing &&
      lhs.seq == rhs.seq;
  }
  uint32_t keep = 0;    // 1 EffectShader.Play, 3 VisualEffect.Play, 5 eyes
  uint32_t form = 0;    // the shader's, the effect's or the texture set's
  float seconds = -1.f; // left, above 0; -1 until its Stop (and the eyes)
  uint32_t facing = 0;  // VisualEffect.Play's facing object as the receiver
                        // numbers it, 0 for None and every other kind
  uint32_t seq = 0;     // the relay's sequence it was kept at
};
