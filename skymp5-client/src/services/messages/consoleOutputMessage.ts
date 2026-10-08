import { MsgType } from "../../messages";

// The line a console command this player sent prints in its console (thuum
// docs/verbs/console-commands.md): the game's own wording where it prints
// one, or why the server refused the command. Server to client.
export interface ConsoleOutputMessage {
    t: MsgType.ConsoleOutput;
    text: string;
    refused: boolean;
}
