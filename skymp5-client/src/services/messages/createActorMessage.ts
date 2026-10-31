import { Appearance } from "../../sync/appearance";
import { Equipment } from "../../sync/equipment";
import { Inventory } from "../../sync/inventory";
import { Transform } from "../../sync/movement";
import { Animation } from "../../sync/animation";
import { MsgType } from "src/messages";

export interface SetNodeTextureSetEntry {
    nodeName: string;
    textureSetId: number;
};

export interface SetNodeScaleEntry {
    nodeName: string;
    scale: number;
};

export interface CustomPropsEntry {
    propName: string;
    propValueJsonDump: string;
};

export interface CreateActorMessage extends CreateActorMessageMainProps {
    t: MsgType.CreateActor,
    idx: number;
    baseRecordType?: "DOOR" | "NPC_" | "MSTT" | "ACTI"; // see PartOne.cpp; MSTT and ACTI come with props.placedBy (thuum docs/verbs/spell-placed-object.md)
    transform: Transform;
    isMe: boolean;
    props?: CreateActorMessageAdditionalProps;
    customPropsJsonDumps: CustomPropsEntry[];
}

export interface CreateActorMessageMainProps {
    refrId?: number;
    baseId?: number;
    appearance?: Appearance;
    equipment?: Equipment;
    animation?: Animation;

    isDead?: boolean;
}

export interface CreateActorMessageAdditionalProps {
    isOpen?: boolean;
    isHarvested?: boolean;
    setNodeTextureSet?: SetNodeTextureSetEntry[];
    setNodeScale?: SetNodeScaleEntry[];
    disabled?: boolean;
    lastAnimation?: string;
    displayName?: string;
    isHostedByOther?: boolean;
    isRaceMenuOpen?: boolean;
    learnedSpells?: number[];
    healRate?: number;
    healRateMult?: number;
    health?: number;
    magickaRate?: number;
    magickaRateMult?: number;
    magicka?: number;
    staminaRate?: number;
    staminaRateMult?: number;
    stamina?: number;
    healthPercentage?: number;
    staminaPercentage?: number;
    magickaPercentage?: number;
    templateChain?: number[];
    inventory?: Inventory;

    isDead?: boolean; // TODO: take a look why doubles CreateActorMessageMainProps

    // thuum docs/verbs/spell-placed-object.md (ADR-057): the server id of the caster whose accepted cast placed this MSTT or ACTI; absent for every other form
    placedBy?: number;
    // thuum docs/verbs/locks.md: the reference's lock as the server holds it;
    // applied to this game's reference instead of the old strip
    lock?: { level: number; key: number; locked: boolean };
}
