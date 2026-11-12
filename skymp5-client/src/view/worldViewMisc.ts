import { Game, ObjectReference, storage } from "skyrimPlatform";
import { WorldView } from "./worldView";
import { SpApiInteractor } from '../services/spApiInteractor';
import { RemoteServer } from "../services/services/remoteServer";

export const getViewFromStorage = (): WorldView | undefined => {
  const res = storage["view"] as WorldView;
  // can't use instanceof here because each hot reload creates a new class
  if (typeof res === "object") {
    return res;
  }
  return undefined;
};

export const localIdToRemoteId = (localFormId: number, newCast: boolean = false): number => {
  if (newCast && localFormId == 0x14) {
    return SpApiInteractor.getControllerInstance().lookupListener(RemoteServer).getMyRemoteRefrId();
  }

  if (localFormId >= 0xff000000) {
    const view = getViewFromStorage();
    if (!view) {
      return 0;
    }
    localFormId = view.getRemoteRefrId(localFormId);
    if (!localFormId) {
      return 0;
    }
    // serverside ids are 64bit
    if (localFormId >= 0x100000000) {
      localFormId -= 0x100000000;
    }
  }
  return localFormId;
};

export const remoteIdToLocalId = (remoteFormId: number): number => {
  if (remoteFormId >= 0xff000000) {
    const view = getViewFromStorage();
    if (!view) {
      return 0;
    }
    remoteFormId = view.getLocalRefrId(remoteFormId);
    if (!remoteFormId) {
      return 0;
    }
  }
  return remoteFormId;
};

// MakeLongFormId's inverse (skymp5-server SpSnippet.cpp): the game-file form
// id a long id carries, the server's id for an actor from the game files it
// holds (the form id plus 0x100000000); 0 for any other id
export const gameFileIdOfLongId = (remoteFormId: number): number => {
  const formId = remoteFormId - 0x100000000;
  return formId > 0 && formId < 0xff000000 ? formId : 0;
};

// thuum train 52, 4.2 (x-acting-player 20261111-034406: c1's relayed
// SetRelationshipRank on Faendal failed, "Unable to find form with id 0"):
// the local form id of a server snippet's self or object argument, for
// SpSnippetService alone. The server writes a game-file actor it holds by its
// long id (SpSnippet::MakeLongFormId), while this client binds such an actor
// as itself under its short id (a FormView keeps the CreateActor's refrId,
// the actor's own form id since npc-puppet, PartOne.cpp), so no view answers
// a long id, on a puppet's seat or a host's alike. The view's answer first,
// as remoteIdToLocalId's, then the reference the long id names, the game
// file's own, which every seat's engine has from the same files whether or
// not a view stands for it. remoteIdToLocalId stays as it is: its other
// callers read 0 as "not in my view"
export const snippetIdToLocalId = (remoteFormId: number): number => {
  return remoteIdToLocalId(remoteFormId) || gameFileIdOfLongId(remoteFormId);
};

// The local form id a form view holds for model index `i`, pure JavaScript
// (no native call): what a network message handler may read, since Skyrim
// Platform refuses natives in that context (thuum docs/verbs/npc-handoff.md
// D6: x-handoff-door 20261024-023055 broke every teleport on a lookup made
// at message time); the native lookup itself waits for the update.
export const getLocalRefrIdOfIndex = (i: number): number => {
  const view = getViewFromStorage();
  if (!view) {
    return 0;
  }
  const formView = view.getFormViews().getNthFormView(i);
  return formView ? formView.getLocalRefrId() : 0;
};

export const getObjectReference = (i: number): ObjectReference | null => {
  const view = getViewFromStorage();
  if (view) {
    const formView = view.getFormViews().getNthFormView(i);
    if (formView) {
      const refrId = formView.getLocalRefrId();
      if (refrId > 0) {
        const refr = ObjectReference.from(Game.getFormEx(refrId));
        if (refr !== null) {
          return refr;
        }
      }
    }
  }
  return null;
};

// thuum docs/verbs/npc-puppet.md: whether a local form id names an actor the
// server knows: a 0xff copy or figure (the view's), or an actor from the game
// files the server sent under its own id (a form in the world model). Any
// other engine actor is nobody's (its hits and activations are not reported).
export const isSyncedActorId = (localFormId: number): boolean => {
  if (localFormId >= 0xff000000) {
    return true;
  }
  if (!localFormId || localFormId === 0x14) {
    return false;
  }
  const world = SpApiInteractor.getControllerInstance().lookupListener(RemoteServer).getWorldModel();
  return world.forms.some((f) => f !== undefined && f.refrId === localFormId);
};
