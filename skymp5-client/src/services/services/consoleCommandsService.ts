import { logError, logTrace } from "../../logging";
import { MsgType } from "../../messages";

// TODO: refactor this out
import { localIdToRemoteId } from "../../view/worldViewMisc";

import { ClientListener, Sp, CombinedController } from "./clientListener";

enum CmdArgument {
    ObjectReference,
    BaseForm,
    Int,
    String,
    // a number the console may type with a fraction: the wire's console
    // argument is an integer or a string, so a fraction travels as its text
    Float,
}

type CmdName = "additem" | "equipitem" | "placeatme" | "disable" | "mp" | "setav" | "modav" | "forceav";

export class ConsoleCommandsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.schemas = ConsoleCommandsService.createSchemas();
        this.setupMpCommand();
        this.setupVanilaCommands();
        this.controller.emitter.on("consoleOutputMessage", (e) => {
            this.sp.printConsole(e.message.text);
        });
    }

    private static createSchemas() {
        const schemas = new Map<CmdName, CmdArgument[]>();
        schemas.set("additem", [CmdArgument.ObjectReference, CmdArgument.BaseForm, CmdArgument.Int]);
        schemas.set("equipitem", [CmdArgument.ObjectReference, CmdArgument.BaseForm]);
        schemas.set("placeatme", [CmdArgument.ObjectReference, CmdArgument.BaseForm]);
        schemas.set("disable", [CmdArgument.ObjectReference]);
        schemas.set("mp", [CmdArgument.ObjectReference, CmdArgument.String]);
        // thuum docs/verbs/console-commands.md: the server's actor value
        // natives (docs/verbs/actor-values.md); Skyrim Platform passes an
        // actor value's name as a string and a float as a number
        schemas.set("setav", [CmdArgument.ObjectReference, CmdArgument.String, CmdArgument.Float]);
        schemas.set("modav", [CmdArgument.ObjectReference, CmdArgument.String, CmdArgument.Float]);
        schemas.set("forceav", [CmdArgument.ObjectReference, CmdArgument.String, CmdArgument.Float]);
        return schemas;
    }

    private setupMpCommand() {
        const command = this.sp.findConsoleCommand(" ConfigureUM") || this.sp.findConsoleCommand("test");
        if (command === null) {
            logError(this, "command was null in setupMpCommand");
            return;
        }

        command.shortName = "mp";
        command.execute = this.getCommandExecutor("mp");
    }

    private setupVanilaCommands() {
        logTrace(this, `Setting up vanila commands`);
        this.schemas.forEach((_, commandName) => {
            logTrace(this, `Setting up command`, commandName);
            const command = this.sp.findConsoleCommand(commandName);
            if (command === null) {
                logError(this, `command`, commandName, `was null in setupVanilaCommands`);
                return;
            }
            if (this.nonVanilaCommands.includes(commandName)) {
                logTrace(this, `command`, commandName, ` is non-vanila command`);
                return;
            }
            command.execute = this.getCommandExecutor(commandName);
        });
        logTrace(this, `Vanila commands set up`);
    }

    private getCommandExecutor(commandName: CmdName): (...args: unknown[]) => boolean {
        return (...args: unknown[]) => {
            // TODO: handle possible exceptions in this function
            const schema = this.schemas.get(commandName);
            if (schema === undefined) {
                logError(this, `Schema not found for command`, commandName);
                return false;
            }

            if (args.length !== schema.length && !this.immuneSchema.includes(commandName)) {
                logError(this, `Mismatch found in the schema of`, commandName, `command`);
                return false;
            }
            for (let i = 0; i < args.length; ++i) {
                switch (schema[i]) {
                    case CmdArgument.ObjectReference:
                        args[i] = localIdToRemoteId(parseInt(`${args[i]}`));
                        break;
                    case CmdArgument.Float:
                        if (typeof args[i] === "number" && !Number.isInteger(args[i])) {
                            args[i] = `${args[i]}`;
                        }
                        break;
                }
            }

            for (let i = 0; i < args.length; ++i) {
                if (typeof args[i] !== "string" && typeof args[i] !== "number") {
                    logError(this, `Bad argument type in command`, commandName, `argument index`, i);
                    return false;
                }
            }

            this.controller.emitter.emit("sendMessage", {
                message: {
                    t: MsgType.ConsoleCommand,
                    data: {
                        commandName,
                        args: args as (string | number)[]
                    }
                },
                reliability: "reliable"
            });

            // the server answers with the line to print (ConsoleOutput)
            return false;
        };
    }

    private readonly schemas: Map<CmdName, CmdArgument[]>;
    private readonly immuneSchema = ["mp"];
    private readonly nonVanilaCommands = ["mp"];
}
