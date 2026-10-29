export enum MsgType {
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
  CellLoaded = 47,
  HostGrant = 48,
  HostRelease = 49,
  ActorSightings = 50, // thuum docs/verbs/npc-puppet.md: a host's roster of engine actors the server does not hold
  ActorFate = 51, // thuum docs/verbs/npc-fate.md (ADR-049): the server's roll for a placement, ahead of the engine's own
  ActorDeath = 52, // thuum docs/verbs/npc-fate.md (ADR-048's B): a host's death report for a game-file actor, checked by the server
  HostedActorState = 53, // thuum docs/verbs/npc-stream.md (ADR-050): a host's values, combat target and package for an actor it simulates
  ActorSpeech = 54, // the same verb's voice slice: a spoken line begins on an actor the host simulates
  DamageCause = 55, // thuum docs/verbs/npc-fate.md (ADR-050 decision 2): the server's number for a hit on an NPC this game hosts, as a cause for the engine
  ActorClaim = 56, // thuum docs/verbs/npc-talk.md (ADR-052): this player's claim on an NPC for a dialogue, or its release
  QuestStage = 57 // thuum docs/verbs/quest-stage.md (ADR-056): a quest stage this game set, reported; the world's stages and the shared list, from the server
}
