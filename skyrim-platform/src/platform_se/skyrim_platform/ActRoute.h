#pragma once

#include <cstdint>

// Where an act on the player runs: a native the scoped column runs on the
// acting player's seat alone (a word taught, a screen effect, a message, a
// crime standing), called by a script this game runs inside another
// player's acting scope (ScriptNatives.cpp, OnStaged). The player it acts on
// is the acting player, whose own game alone may change it, so this game
// never runs it. From a press or a trigger entry this game runs for its
// presser (the scope's root kTrigger), the acting player's own game never
// ran the script, so the call is forwarded for the server to run there
// (wire-rules natives' Audience::Acting). From a stage or a word another
// game set (the root kRelayed), that game ran the same script and made the
// act itself, so the call is skipped here and sent nowhere. Outside such a
// scope, or for any other row, the call runs here.
namespace ActRoute {

enum class Route
{
  kRun,
  kSkip,
  kForward
};

struct Facts
{
  // the row's scoped column is the acting seat alone
  bool actingSeatAlone = false;
  // the call runs inside another player's acting scope (ActingScope::Swaps)
  bool otherPlayerActs = false;
  // the scope's root is a press or an entry this game runs for its presser
  bool pressRoot = false;
};

inline Route Of(const Facts& f)
{
  if (!f.actingSeatAlone || !f.otherPlayerActs) {
    return Route::kRun;
  }
  return f.pressRoot ? Route::kForward : Route::kSkip;
}

// The self a forwarded act names: the player (0x14, which the server reads
// as the acting player) for a static on no object or for a method on the
// acting player's figure, where the script named the player; the frame's
// self otherwise (a form's: an image space modifier, a faction)
inline uint32_t SelfOf(bool staticRow, uint32_t frameSelf, uint32_t figure)
{
  if (staticRow || (figure != 0 && frameSelf == figure)) {
    return 0x14;
  }
  return frameSelf;
}

}
