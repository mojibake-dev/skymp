import { MsgType } from "../../messages";

// The world's game clock as the engine's six time globals hold it (thuum
// docs/verbs/time.md): at login, ahead of the player's own CreateActor, and
// every 60 s after.
export interface SetGameTimeMessage {
    t: MsgType.SetGameTime;
    year: number;
    month: number; // from 0, Morning Star
    day: number; // from 1
    hour: number;
    daysPassed: number;
    timeScale: number; // game seconds per real second
}
