import { Appearance } from "../../sync/appearance";
import { Equipment } from "../../sync/equipment";
import { Inventory } from "../../sync/inventory";
import { Transform } from "../../sync/movement";
import { Animation } from "../../sync/animation";
import { RaceOverlay } from "./raceOverlay";
import { ServedLookProp } from "./servedLookProp";
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
    // the server's name for the reference's disabled flag (CreateActorMessage.h,
    // wire-schema's CreateActor); the client read `disabled`, an upstream slip
    // since 63ca3d82, and applied it over the engine's own state. Nothing
    // applies it now: a reference's engine state is the server's
    // DestructionStage word (thuum docs/DECISIONS.md ADR-068 amendment B,
    // RefStateService)
    isDisabled?: boolean;
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

    // thuum docs/verbs/character-creation.md, the race permit: on the
    // player's own actor, the races the server restricts and the ones this
    // profile may wear (global race form ids); absent or empty for a profile
    // with no permit
    restrictedRaces?: number[];
    permittedRaces?: number[];

    isDead?: boolean; // TODO: take a look why doubles CreateActorMessageMainProps

    // thuum docs/verbs/spell-placed-object.md (ADR-057): the server id of the caster whose accepted cast placed this MSTT or ACTI; absent for every other form
    placedBy?: number;
    // thuum docs/verbs/locks.md: the reference's lock as the server holds it;
    // applied to this game's reference instead of the old strip
    lock?: { level: number; key: number; locked: boolean };
    // thuum ADR-068 rung 5, D-T1 A (sync/raceOverlay.ts): on another player's
    // figure, what its non-Playable, non-twin race's scripts added; absent
    // for none. Never on the player's own CreateActor (a relaunch mid-form
    // logs in as the player's own race)
    overlay?: RaceOverlay;
    // thuum #52 (wire-schema ServedLook, SCHEMA_VERSION 53): the looks the
    // actor's simulator's script drew on it and has not stopped, as the
    // server keeps them, in the order kept; never on a player's own, never
    // to the actor's simulator. keep 1 a shader's Play, 3 a visual effect's,
    // 5 an eye texture; seconds left, -1 until its Stop (and the eyes);
    // facing as this engine numbers it
    looks?: ServedLookProp[];

    // thuum docs/verbs/summon.md (tracker #84): on the summon owner's create
    // word alone (sent to no other listener), the engine reference the word's
    // form is: the summon's runtime id in this game's engine (the localId the
    // owner's Summon report named), which FormView binds instead of placing a
    // copy. Absent on every other create word, the other games' copies of the
    // summon included
    ownLocal?: number;
    // thuum docs/verbs/summon.md (tracker #111, SCHEMA_VERSION 59): on a
    // summon's create word to every listener but its caster, within the
    // record's arrival window, the summon effect (a load-order MGEF) whose
    // arrival the copy is: FormView plays that effect's own hit-effect art on
    // the copy at its create (the engine's SummonPlacementEffect draws the
    // same art on the caster's game). Absent on the caster's word, which
    // binds instead, and on a later loader's
    summonEffect?: number;
    // docs/verbs/difficulty.md (SCHEMA_VERSION 75): the server's difficulty,
    // 0 (Novice) to 5 (Legendary), on the player's own create word alone:
    // DisableDifficultySelectionService writes it to the player's own
    // difficulty field, which the engine's DamageActor indexes
    difficulty?: number;
}
