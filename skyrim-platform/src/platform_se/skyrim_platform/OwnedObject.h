#pragma once

// Two rules for a world object this game owns (its cell's host), whose
// scripts run here for every player (ScriptNatives.cpp, OnCall).
namespace OwnedObject {

// Whether an owned latent action (PlayAnimationAndWait on a world object) is
// answered at once, its play kept and shown on this game's graph
// (ObjectAnimationService), instead of waiting on this game's engine: when
// the reference's 3D is not loaded here, the graph never raises the wait's
// end event.
inline bool AnswersAtOnce(bool latentAction, bool actor, bool loaded3D,
                          bool otherPlayersPress)
{
  (void)otherPlayersPress;
  return latentAction && !actor && !loaded3D;
}

// Whether a look-only row's call (a sound, a noise) on a world object this
// game owns, made by that object's own script, crosses as a look to the
// games that render the object. A trap family trigger's crosses on its own
// path (OnFamilyCall); any other stays local.
inline bool OwnLookCrosses(bool owned, bool callerIsSubject)
{
  (void)owned;
  (void)callerIsSubject;
  return false;
}

}
