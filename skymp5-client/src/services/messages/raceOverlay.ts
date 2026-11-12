// thuum docs/DECISIONS.md ADR-068 rung 5, D-T1 A (docs/research/
// rung5-quests-2026-11-09.md 8(b)): a player's race overlay as the wire
// carries it on that player's figure, CreateActor's props.overlay and
// UpdateAppearance's overlay; absent while the player wears its record. The
// pair of skymp5-server/cpp/messages/RaceOverlay.h, shared by both messages
// as scriptStateTypes.ts is by ScriptState's two. JSON field names as
// wire-schema's RaceOverlay reads them (the server's row, wire-rules
// player_own::Overlay: open, race, base, spells, shouts, items);
// sync/raceOverlay.ts parses and applies it
export interface RaceOverlay {
  open?: boolean;   // false: no overlay (the other fields empty)
  race?: number;    // the race worn beyond the record, which the figure wears
  base?: number;    // the recorded race it returns to (the server's)
  spells: number[]; // the spells the race's scripts added (abilities)
  shouts: number[]; // the shouts they added (the howls)
  items: number[];  // the items they equipped (WolfSkinFXArmor)
}
