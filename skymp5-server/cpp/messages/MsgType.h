#pragma once
#include <cstdint>

enum class MsgType : uint8_t
{
  Invalid = 0,
  CustomPacket = 1,
  UpdateMovement = 2,
  UpdateAnimation = 3,
  UpdateAppearance = 4,
  UpdateEquipment = 5,
  Activate = 6,
  UpdateProperty = 7,
  PutItem = 8,
  TakeItem = 9,
  FinishSpSnippet = 10,
  OnEquip = 11,
  ConsoleCommand = 12,
  CraftItem = 13,
  Host = 14,
  CustomEvent = 15,
  ChangeValues = 16,
  OnHit = 17,
  DeathStateContainer = 18,
  DropItem = 19,
  Teleport = 20,
  OpenContainer = 21,
  PlayerBowShot = 22,
  SpellCast = 23,
  UpdateAnimVariables = 24,

  // ex-strings
  DestroyActor = 25,
  HostStart = 26,
  HostStop = 27,
  SetInventory = 28,
  SetRaceMenuOpen = 29,
  SpSnippet = 30,
  Teleport2 = 31,
  UpdateGamemodeData = 32,
  CreateActor = 33,

  // thuum's, appended after SkyMP's (skymp-wire wire-schema)
  SetGameTime = 34,
  RestIntent = 35,
  MapMarkerDiscovered = 36,
  IngredientEffectsKnown = 37,
  Favorites = 38,
  RaceMenuPreset = 39,
  ActorValues = 40,
  ConsoleOutput = 41,
  ArrowShot = 42,
  MagicEffects = 43,
  UpdateAction = 44,
  PlayerGlobals = 45,
  ActorJump = 46,
  CellLoaded = 47,     // thuum docs/verbs/cell-host.md
  HostGrant = 48,      // the same, the reserved wire family's id 7
  HostRelease = 49,    // the same, id 8
  ActorSightings = 50, // thuum docs/verbs/npc-puppet.md: a host's roster of
                       // game-file actors the server does not hold
  ActorFate = 51,      // thuum docs/verbs/npc-fate.md (ADR-049): the server's
                       // roll for a placement, ahead of the engine's own
  ActorDeath = 52,     // thuum docs/verbs/npc-fate.md (ADR-048's B): a host's
                       // death report for a game-file actor, checked
  HostedActorState = 53, // thuum docs/verbs/npc-stream.md (ADR-050): a host's
                         // values, combat target and package for an actor it
                         // simulates
  ActorSpeech = 54,      // the same verb's voice slice: a spoken line begins
                         // on an actor the host simulates
  DamageCause = 55,      // thuum docs/verbs/npc-fate.md (ADR-050 decision 2):
                         // the server's number for a hit on a hosted NPC,
                         // to its host as a cause (the resulting health)
  ActorClaim = 56,       // thuum docs/verbs/npc-talk.md (ADR-052): a player's
                         // claim on an NPC for a dialogue, or its release
  QuestStage = 57,       // thuum docs/verbs/quest-stage.md (ADR-056): a quest
                         // stage the world shares: the report up, the relay
                         // and the login replay down
  PropTransform = 58,    // thuum docs/verbs/prop-physics.md (ADR-058): a
                         // physics prop's transform from the one seat that
                         // simulates it, relayed to the cell's other seats
  PropClaim = 59,        // the same verb: a player touched a prop another
                         // seat simulates (contact, grab, drop, impulse)
  PropOwner = 60,        // the same verb: who simulates a prop from now, to
                         // every listener at each owner change

  PlacedRequest = 61, // thuum docs/verbs/spell-placed-object.md (ADR-057
                      // shape A): the server asks the caster's game where
                      // its engine stands a listed spell's placed objects
  PlacedReport = 62,  // the same verb: the caster's answer for one base,
                      // validated and placed for every other seat
  SkillUse = 63, // thuum docs/verbs/leveling.md (M5): a player's skill uses
                 // of the last second, batched, for the server's formulas
  Max
};
