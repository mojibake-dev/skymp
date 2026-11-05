import { Movement } from "../sync/movement";
import { Animation } from "../sync/animation";
import { Appearance } from "../sync/appearance";
import { Equipment } from "../sync/equipment";
import { Inventory } from "../sync/inventory";
import { CreateActorMessageMainProps, CreateActorMessageAdditionalProps } from "src/services/messages/createActorMessage";
import { UpdateActionMessage } from "src/services/messages/updateActionMessage";

// Own properties (not inherited) are being assigned locally
export interface FormModel extends CreateActorMessageAdditionalProps, CreateActorMessageMainProps{
  idx?: number;
  movement?: Movement;
  numMovementChanges?: number;
  numAppearanceChanges?: number;
  isMyClone?: boolean;
  // thuum docs/verbs/spell-cast.md (ADR-031): every animation event the
  // server relayed since the view last applied them, oldest first. `animation`
  // alone kept the newest, so a concentration cast's start and its release
  // arriving in one of this game's frames left the release alone, which the
  // figure's graph refuses (x-recorder-flames 20261014-081353)
  animationQueue?: Animation[];
  // thuum docs/verbs/cast-action.md: the actions the server relayed for
  // this actor since the view last performed them, oldest first; the view
  // plays one per two updates (its variables, then the action)
  actionQueue?: UpdateActionMessage[];
}

export interface WorldModel {
  forms: Array<FormModel | undefined>;
  playerCharacterFormIdx: number;
  playerCharacterRefrId: number;
}
