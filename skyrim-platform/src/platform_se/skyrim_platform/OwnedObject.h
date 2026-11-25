#pragma once

// Two rules for a world object this game owns (its cell's host), whose
// scripts run here for every player (ScriptNatives.cpp, OnCall).
namespace OwnedObject {

// Whether an owned latent action (PlayAnimationAndWait on a world object) is
// answered at once, its play kept and shown on this game's graph
// (ObjectAnimationService), instead of waiting on this game's engine: when
// the reference's 3D is not loaded here, the graph never raises the wait's
// end event; and when another player's press or entry started the script,
// the presser's game renders the object while this game's player may be
// away, and this game's engine accepts the graph event but does not advance
// the graph until its own player comes near (the wait would hold the script
// past every later press).
inline bool AnswersAtOnce(bool latentAction, bool actor, bool loaded3D,
                          bool otherPlayersPress)
{
  return latentAction && !actor && (!loaded3D || otherPlayersPress);
}

// Whether a look-only row's call (a sound, a noise) on a world object this
// game owns, made by that object's own script, crosses as a look to the
// games that render the object, as its animations do: the owner's script
// runs here alone, so its sound plays nowhere else otherwise. A trap family
// trigger's crosses on its own path (OnFamilyCall); another script's call on
// the object, or one on an object another game owns, stays local.
inline bool OwnLookCrosses(bool owned, bool callerIsSubject)
{
  return owned && callerIsSubject;
}

}
