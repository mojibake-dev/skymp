import { MsgType } from "../../messages";
import { Appearance } from "../../sync/appearance";
import { RaceOverlay } from "./raceOverlay";

export interface UpdateAppearanceMessage {
    t: MsgType.UpdateAppearance;
    idx: number;
    data: Appearance | null;
    // thuum ADR-068 rung 5, D-T1 A (sync/raceOverlay.ts): from the server, the
    // race overlay that holds with this look; absent for none (it ends with
    // the race's return)
    overlay?: RaceOverlay;
}
