import { Game } from "skyrimPlatform";
import { ObjectReferenceEx } from "../extensions/objectReferenceEx";

export class PlayerCharacterDataHolder {
  static updateData() {
    const player = Game.getPlayer();
    if (!player) {
      return;
    }
    
    this.inJumpState = player.getAnimationVariableBool("bInJumpState");
    this.worldOrCell = ObjectReferenceEx.getWorldOrCell(player);
    this.pos = [player.getPositionX(), player.getPositionY(), player.getPositionZ()];
    this.crosshairRefId = Game.getCurrentCrosshairRef()?.getFormID() || 0;
  }

  static isInJumpState() {
    return this.inJumpState;
  }

  static getWorldOrCell() {
    return this.worldOrCell;
  }

  static getCrosshairRefId() {
    return this.crosshairRefId;
  }

  // the player's position as this frame read it, once for every view
  static getPos(): [number, number, number] | undefined {
    return this.pos;
  }

  private static inJumpState = false;
  private static worldOrCell = 0;
  private static crosshairRefId = 0;
  private static pos: [number, number, number] | undefined = undefined;
}
