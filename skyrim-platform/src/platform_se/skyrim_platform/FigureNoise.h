#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

// Whose noise an actor's detection hears as the player's (the hearing hooks
// on CalculateDetection, Hooks.cpp). Out of combat the engine reads a
// target's noise (GetActorsDetectionEvent, Address Library 39285) only when
// the target is its own player and the detector is hostile to it
// (Actor::GetHostileToActor, 37537); the player's noise carries
// fDetectEventDistancePlayer (1536) where another actor's carries
// fDetectEventDistanceNPC (512); and the player's projectiles in flight are
// watched out of combat at the player's distance, another actor's only in
// combat. A detector in combat sees a target that is neither the player
// nor its own combat target at fDetectionCombatNonTargetDistanceMult's
// share of its sight distance, and the player always at the whole of it.
// Another player's figure is that player on this game, so its noise, its
// arrows and its body reach this game's actors as the player's do.
namespace FigureNoise {

struct Target
{
  // the target is this game's own player
  bool isEnginePlayer = false;
  // the target is another player's figure
  bool isPlayerFigure = false;
};

// whether detection reads the target as it reads its own player: the
// engine's own player, and another player's figure
inline bool CountsAsPlayer(const Target& t)
{
  return t.isEnginePlayer || t.isPlayerFigure;
}

// what the engine's `cmp r14, rcx` compares the target with at the noise's
// distance and at the projectile watch, in place of the player its own
// instruction loaded: the target itself when it counts as the player, so
// the compare holds, else the player
inline std::uintptr_t ComparedWith(const Target& t, std::uintptr_t target,
                                   std::uintptr_t player)
{
  return CountsAsPlayer(t) ? target : player;
}

// CalculateDetection (Address Library 37774, 0x690870 on 1.6.1170), read on
// sky-re through REA: the four places it tests the target (r14) against
// the player (the global at RIP + disp, Address Library 401069), with the
// bytes each site holds; rsi is the detector
//
// the in-combat sight test, after the target's sight distance is in xmm6:
//   +0x16a  4c 3b 35 d32   cmp  r14, [player]
//   +0x171  74 3b          jz   +0x1ae (the whole sight distance)
//   +0x173  ...            the detector's IsInCombat (vtable 0E3), its flag
//                          +0xe8 bit 14, its combat target (+0x104) against
//                          the target's handle, each jumping to +0x1ae
//   +0x1a6                 mulss xmm6, fDetectionCombatNonTargetDistanceMult
// The site's write keeps the jump's target and turns its test into the
// answer of a call: `call` the stub (5 bytes, passing the target), then
// `test al, al` and `jnz` +0x1ae, so a target that counts as the player is
// seen at the whole distance. Volatile registers are dead there: the call
// before the site ends them, and both of its paths write them before
// reading
inline constexpr std::size_t kCombatSight = 0x16a;
inline constexpr std::array<std::uint8_t, 9> kCombatSightBytes = {
  0x4c, 0x3b, 0x35, 0xb7, 0x6c, 0xaa, 0x02, 0x74, 0x3b
};
// the bytes after the call: `test al, al`, then `jnz` with the engine's
// own jump's displacement, which stays at the same place
inline constexpr std::array<std::uint8_t, 4> kCombatSightTail = {
  0x84, 0xc0, 0x75, kCombatSightBytes[8]
};
//
// the gate before the target's noise is read, entered only when neither
// actor has a combat controller (+0x160) and the fight reaction is neither
// ally nor friend:
//   +0x5ff  4c 3b 35 d32   cmp  r14, [player]
//   +0x606  0f 85 d32      jnz  the end (no noise read)
//   +0x60c  49 8b d6       mov  rdx, r14
//   +0x60f  48 8b ce       mov  rcx, rsi
//   +0x612  e8 d32         call Actor::GetHostileToActor
//   +0x617  84 c0          test al, al
inline constexpr std::size_t kGate = 0x5ff;
inline constexpr std::size_t kGateJump = 0x606;
inline constexpr std::size_t kGateJumpSize = 6;
inline constexpr std::size_t kGateCall = 0x612;
inline constexpr std::array<std::uint8_t, 26> kGateBytes = {
  0x4c, 0x3b, 0x35, 0x22, 0x68, 0xaa, 0x02, 0x0f, 0x85, 0x98, 0x05, 0x00, 0x00,
  0x49, 0x8b, 0xd6, 0x48, 0x8b, 0xce, 0xe8, 0x89, 0x90, 0xfe, 0xff, 0x84, 0xc0
};
// the noise's distance, fDetectEventDistancePlayer for the player:
//   +0x637  48 8b 0d d32   mov  rcx, [player]
//   +0x63e  4c 3b f1       cmp  r14, rcx
//   +0x641  75 0a          jnz  the NPC distance
inline constexpr std::size_t kNoiseDistance = 0x637;
inline constexpr std::array<std::uint8_t, 12> kNoiseDistanceBytes = {
  0x48, 0x8b, 0x0d, 0xea, 0x67, 0xaa, 0x02, 0x4c, 0x3b, 0xf1, 0x75, 0x0a
};
// the projectile watch, reached also by a jump from +0x6a4 straight to its
// compare at +0x7df, which then compares with the rcx the noise's distance
// site loaded: both sites' writes put ComparedWith's pointer in rcx, so
// that compare answers as the watch's own site would
//   +0x7d8  48 8b 0d d32   mov  rcx, [player]
//   +0x7df  4c 3b f1       cmp  r14, rcx
//   +0x7e2  74 18          jz   the player's watch distance
inline constexpr std::size_t kProjectileWatch = 0x7d8;
inline constexpr std::array<std::uint8_t, 12> kProjectileWatchBytes = {
  0x48, 0x8b, 0x0d, 0x49, 0x66, 0xaa, 0x02, 0x4c, 0x3b, 0xf1, 0x74, 0x18
};
// the bytes the hooks read, from the function's entry
inline constexpr std::size_t kCodeSize = kProjectileWatch + 12;

// a `mov rcx, [player]` site's write: a call to the stub (5 bytes, the
// trampoline's) and then `xchg rax, rcx`, so rcx holds what ComparedWith
// returned, rax being free there (it is written before it is read)
inline constexpr std::array<std::uint8_t, 2> kXchgRaxRcx = { 0x48, 0x91 };

// the stub's bytes: `mov rcx, r14` passes the target as the first
// argument, and `jmp [rip]` goes to the function at `fn`, which returns to
// the site
inline std::array<std::uint8_t, 17> Stub(std::uint64_t fn)
{
  std::array<std::uint8_t, 17> s = { 0x49, 0x8b, 0xce, 0xff, 0x25,
                                     0x00, 0x00, 0x00, 0x00 };
  std::memcpy(s.data() + 9, &fn, sizeof(fn));
  return s;
}

struct Found
{
  // the global the engine reads its player from
  std::uintptr_t playerSlot = 0;
  // the function the gate calls (Actor::GetHostileToActor's address)
  std::uintptr_t hostileCall = 0;
  // where the in-combat sight test's jump lands: past the share, at the
  // whole sight distance
  std::uintptr_t combatSightWhole = 0;
};

namespace Detail {
inline std::int32_t Disp(const std::uint8_t* at)
{
  std::int32_t d;
  std::memcpy(&d, at, sizeof(d));
  return d;
}

template <std::size_t N>
bool Holds(const std::uint8_t* code, std::size_t offset,
           const std::array<std::uint8_t, N>& bytes)
{
  return std::memcmp(code + offset, bytes.data(), N) == 0;
}
}

// the three sites read from the function's code (kCodeSize bytes from its
// entry, the function at `entry`): the player's global and the gate's
// callee, both RIP-relative, or nothing when a site's bytes differ from the
// ones above, and then no site is written
inline std::optional<Found> Find(const std::uint8_t* code,
                                 std::uintptr_t entry)
{
  if (!Detail::Holds(code, kGate, kGateBytes) ||
      !Detail::Holds(code, kNoiseDistance, kNoiseDistanceBytes) ||
      !Detail::Holds(code, kProjectileWatch, kProjectileWatchBytes)) {
    return std::nullopt;
  }
  const auto ripTarget = [&](std::size_t insn, std::size_t dispAt,
                             std::size_t size) {
    return entry + insn + size + Detail::Disp(code + insn + dispAt);
  };
  Found f;
  f.playerSlot = ripTarget(kGate, 3, 7);
  f.hostileCall = ripTarget(kGateCall, 1, 5);
  return f;
}

}
