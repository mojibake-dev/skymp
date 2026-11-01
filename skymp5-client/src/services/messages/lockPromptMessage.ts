import { MsgType } from "../../messages";

// thuum docs/verbs/locks.md (M4): the server tells this game that its
// activation met a lock without the key, so the game runs the engine's own
// activation on its locked reference (the lockpicking menu, or the "requires
// key" box). Server to client, reliable, the activating player alone.
export interface LockPromptMessage {
    t: MsgType.LockPrompt;
    refr: number;
}
