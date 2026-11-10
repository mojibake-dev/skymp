#pragma once
#include <cstdint>
#include <tuple>
#include <vector>

// thuum ADR-068 rung 5 (D-T1 A; docs/research/rung5-quests-2026-11-09.md
// section 8(b)): a player's race overlay, the race it wears beyond its
// record (WerewolfBeastRace and its kind: neither Playable nor a twin of the
// recorded race) and what its scripts added meanwhile, never written to its
// change form. The server carries it on that player's figure: CreateActor's
// props.overlay and UpdateAppearance's overlay, another player's figure
// alone (wire-schema skymp.rs RaceOverlay; wire-rules player_own::Overlay,
// wire-bridge PlayerOverlayRow). Absent while the player wears its record.
struct RaceOverlay
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("open", open)
      .Serialize("race", race)
      .Serialize("base", base)
      .Serialize("spells", spells)
      .Serialize("shouts", shouts)
      .Serialize("items", items);
  }

  auto ToTuple() const
  {
    return std::make_tuple(open, race, base, spells, shouts, items);
  }

  friend bool operator==(const RaceOverlay& lhs, const RaceOverlay& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  bool open = false;            // false names none, every field empty
  uint32_t race = 0;            // the race worn
  uint32_t base = 0;            // the recorded race its end returns to
  std::vector<uint32_t> spells; // added while open, in order
  std::vector<uint32_t> shouts; // added while open, in order
  std::vector<uint32_t> items;  // equipped while open, in order
};
