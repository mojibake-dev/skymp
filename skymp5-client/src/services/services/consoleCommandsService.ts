import { logError, logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";

// TODO: refactor this out
import { localIdToRemoteId } from "../../view/worldViewMisc";

import { QuestStartStopEvent } from "skyrimPlatform";
import { ClientListener, Sp, CombinedController } from "./clientListener";

// thuum ADR-068 rung 5, D-Q4 C: the quest commands' marks (setupQuestCommands)
const QUEST_STAGE_TAG = "verb:quest-stage";
// a run word's stage in the platform's QuestCause table: a start, a stop
const QUEST_START = -1;
const QUEST_STOP = -2;
// a typed command's mark's life: the engine queues a typed start or stop to
// the story teller's next pass, so the word's event comes after the handler
// (a static read of the commands' handlers, HYPOTHESIS), and a console left
// open may hold that pass back; a mark no event takes expires on its own
const CONSOLE_MARK_MS = 30000;
// the quests whose run questRuns keeps (this game's own questStart and
// questStop events since its last load), the oldest let go first
const QUEST_RUNS_MAX = 4096;

enum CmdArgument {
    ObjectReference,
    BaseForm,
    Int,
    String,
    // a number the console may type with a fraction: the wire's console
    // argument is an integer or a string, so a fraction travels as its text
    Float,
}

type CmdName = "additem" | "equipitem" | "placeatme" | "disable" | "mp" | "setav" | "modav" | "forceav"
    | "removeitem" | "enable" | "kill" | "resurrect" | "setpos" | "setangle" | "moveto" | "coc"
    | "teachword" | "unlockword" | "addshout" | "addspell" | "lock" | "unlock"
    | "addperk" | "removeperk" | "setlevel";

export class ConsoleCommandsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.schemas = ConsoleCommandsService.createSchemas();
        this.setupMpCommand();
        this.setupVanilaCommands();
        this.setupServerOnlyCommands();
        this.setupQuestCommands();
        this.controller.on("questStart", (e) => this.noteQuestRun(e, true));
        this.controller.on("questStop", (e) => this.noteQuestRun(e, false));
        this.controller.emitter.on("gameLoad", () => this.questRuns.clear());
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
        // thuum docs/verbs/locks.md: the server's lock state; the game's own
        // lock and unlock would change this game alone
        schemas.set("lock", [CmdArgument.ObjectReference, CmdArgument.Int]);
        schemas.set("unlock", [CmdArgument.ObjectReference]);
        // thuum docs/verbs/perks.md: the record's perks and the level the perk
        // points come from; the game's own addperk would change this game alone
        schemas.set("addperk", [CmdArgument.ObjectReference, CmdArgument.BaseForm]);
        schemas.set("removeperk", [CmdArgument.ObjectReference, CmdArgument.BaseForm]);
        schemas.set("setlevel", [CmdArgument.ObjectReference, CmdArgument.Int]);
        schemas.set("mp", [CmdArgument.ObjectReference, CmdArgument.String]);
        // thuum docs/verbs/console-commands.md: the server's actor value
        // natives (docs/verbs/actor-values.md); Skyrim Platform passes an
        // actor value's name as a string and a float as a number
        schemas.set("setav", [CmdArgument.ObjectReference, CmdArgument.String, CmdArgument.Float]);
        schemas.set("modav", [CmdArgument.ObjectReference, CmdArgument.String, CmdArgument.Float]);
        schemas.set("forceav", [CmdArgument.ObjectReference, CmdArgument.String, CmdArgument.Float]);
        // the server's own paths (console-commands.md, "Commands, first
        // set"): the inventory, references, death and position verbs. Kill
        // may name a killer, Resurrect an animation flag and MoveTo offsets,
        // so their argument counts vary (immuneSchema)
        schemas.set("removeitem", [CmdArgument.ObjectReference, CmdArgument.BaseForm, CmdArgument.Int]);
        schemas.set("enable", [CmdArgument.ObjectReference]);
        schemas.set("kill", [CmdArgument.ObjectReference, CmdArgument.ObjectReference]);
        schemas.set("resurrect", [CmdArgument.ObjectReference, CmdArgument.Int]);
        schemas.set("setpos", [CmdArgument.ObjectReference, CmdArgument.String, CmdArgument.Float]);
        schemas.set("setangle", [CmdArgument.ObjectReference, CmdArgument.String, CmdArgument.Float]);
        schemas.set("moveto", [CmdArgument.ObjectReference, CmdArgument.ObjectReference]);
        // COC: the cell as Skyrim Platform passes it, the typed name or the
        // form id the game found for that name; the server permits the jump
        // and has this game make it (Debug.CenterOnCell)
        schemas.set("coc", [CmdArgument.ObjectReference, CmdArgument.String]);
        // thuum docs/verbs/shouts.md: what a player knows of shouts is the
        // server's record; typed here they used to change this game alone
        schemas.set("teachword", [CmdArgument.ObjectReference, CmdArgument.BaseForm]);
        schemas.set("unlockword", [CmdArgument.ObjectReference, CmdArgument.BaseForm]);
        schemas.set("addshout", [CmdArgument.ObjectReference, CmdArgument.BaseForm]);
        // thuum docs/verbs/cast-teleport.md: a learned spell is the server's
        // record too
        schemas.set("addspell", [CmdArgument.ObjectReference, CmdArgument.BaseForm]);
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

    // thuum docs/verbs/console-commands.md: commands whose effect only the
    // server may make, which it does not run yet or never runs (a save or a
    // load). The game never runs them here: Skyrim Platform runs a replaced
    // command's own handler only when execute returns true (ConsoleApi.cpp,
    // ConsoleComand_Execute). The client sends the name alone; the server's
    // table (wire-rules console) decides and answers with the line to print.
    private setupServerOnlyCommands() {
        ConsoleCommandsService.serverOnlyCommands.forEach((commandName) => {
            const command = this.sp.findConsoleCommand(commandName);
            if (command === null) {
                logError(this, `command`, commandName, `was null in setupServerOnlyCommands`);
                return;
            }
            command.execute = () => {
                this.controller.emitter.emit("sendMessage", {
                    message: {
                        t: MsgType.ConsoleCommand,
                        data: { commandName, args: [] }
                    },
                    reliability: "reliable"
                });
                return false;
            };
        });
    }

    // thuum docs/DECISIONS.md ADR-068 rung 5, D-Q4 C: a quest command the
    // player types is the player's own deliberate act, so the run word or
    // stage-0 report it makes is the world's. It runs no script, so the
    // platform's seam reads no root for it, and its words would be the
    // seat's own (QuestStageService holds those). Skyrim Platform runs this
    // execute before the engine's own handler, which runs only when execute
    // answers true (ConsoleApi.cpp, ConsoleComand_Execute), so the mark comes
    // first: markQuestCause puts the world's verdict, root
    // "console.<command>", on each run word the command will make and on
    // setstage's stage, in the platform's QuestCause table for 30 s; the
    // word's next event takes it once, the stage's event reads it, and the
    // fragment the stage runs carries it to what that sets. A start is marked
    // only for a quest not running here and a stop only for a running one,
    // the seam's own rule (ScriptNatives.cpp MarkQuestCause: a Start of a
    // running quest or a Stop of a stopped one changes nothing and raises no
    // event), since an untaken mark lends the world's cause for its 30 s to
    // the quest's next engine start or stop and to every stage the engine
    // sets meanwhile (QuestCause::OfStage; the fresh review of rung 5's
    // second head, note 5: setstage on a running quest marked a start that
    // never came). The run is questRuns' (noteQuestRun): this execute runs
    // outside the Papyrus VM's update, where no game function may be called
    // (Skyrim Platform's docs/skyrim_platform/features.md:31; CallNativeApi.cpp
    // refuses "can't be called in this context" while main.cpp's OnUpdate has
    // not set its VM), so Quest.IsRunning cannot be asked here. A quest no
    // event named since this game's load is unread, and its command marks
    // every run word it may make, as before. The engine's own
    // starts (a load, Start Game Enabled, the Story Manager) mark nothing and
    // stay the seat's own. Execute answers true whatever the mark did, so the
    // engine runs the command as typed
    private setupQuestCommands() {
        ConsoleCommandsService.questCommands.forEach((words, commandName) => {
            const command = this.sp.findConsoleCommand(commandName);
            if (command === null) {
                logError(this, `command`, commandName, `was null in setupQuestCommands`);
                return;
            }
            command.execute = (...args: unknown[]) => {
                try {
                    this.markTypedQuest(commandName, words, args);
                } catch (e) {
                    logError(this, `marking`, commandName, e);
                }
                return true;
            };
        });
    }

    // args[0] the selected reference (0 for none); args[1] the quest as
    // Skyrim Platform found it, its form id for an editor id or a hex id the
    // game knows, else the typed text, which marks nothing (the engine finds
    // no quest either); args[2] setstage's stage. Of the run words the
    // command may make, the ones its quest's run here says it will make
    // (setupQuestCommands), so setstage on a running quest marks its stage
    // alone. One line a command, naming what it marked and the run it read
    private markTypedQuest(commandName: string, words: number[], args: unknown[]) {
        const questId = args[1];
        if (typeof questId !== "number" || !Number.isInteger(questId) || questId <= 0) {
            logTrace(this, `console`, commandName, `names no quest form here (${String(questId)}): nothing armed`);
            return;
        }
        if (typeof this.sp.markQuestCause !== "function") {
            if (!this.questMarkMissingSaid) {
                this.questMarkMissingSaid = true;
                logLab(QUEST_STAGE_TAG, `console quest commands arm nothing: this platform has no markQuestCause`);
            }
            return;
        }
        const stage = args[2];
        const typedStage = commandName === "setstage" && typeof stage === "number" && Number.isInteger(stage) && stage >= 0 && stage <= 0xffff;
        const running = this.questRuns.get(questId);
        const made = words.filter((word) => (word === QUEST_START ? running !== true : running !== false));
        const marks = typedStage ? [stage, ...made] : made;
        const run = running === undefined ? "its run unread here" : running ? "running here" : "stopped here";
        const id = questId.toString(16);
        if (marks.length === 0) {
            logLab(QUEST_STAGE_TAG, `console ${commandName} ${id}: nothing armed (${run}, so it makes no ${words.includes(QUEST_STOP) ? "stop" : "start"})`);
            return;
        }
        const root = `console.${commandName}`;
        const refused = marks.filter((mark) => !this.sp.markQuestCause(questId, mark, "world", root, CONSOLE_MARK_MS));
        if (refused.length > 0) {
            logError(this, `console`, commandName, id, `: the platform refused its mark(s)`, refused.join(", "));
            return;
        }
        logLab(QUEST_STAGE_TAG, `console ${commandName} ${id}: armed world for its next event (${marks.map(ConsoleCommandsService.markName).join(", ")}; ${run})`);
    }

    // a mark as the arm's line names it
    private static markName(mark: number): string {
        return mark === QUEST_START ? "start" : mark === QUEST_STOP ? "stop" : `stage ${mark}`;
    }

    // questRuns: a quest's run as this game's own questStart and questStop
    // events last said it (the platform raises them in an update, where
    // Quest.getFormID may be called). HYPOTHESIS: their run is the flag the
    // seam reads, TESQuest::IsEnabled (kEnabled, CommonLibSSE-NG
    // src/RE/T/TESQuest.cpp:42-45, ScriptNatives.cpp:1617), every start or
    // stop after a load raises one, and its event reaches this game's update
    // before the next typed command; CommonLib's own TESQuest::IsRunning
    // (:47-50) reads another expression, and Papyrus's Quest.IsRunning,
    // which a console execute may not call, is unread against either
    private noteQuestRun(e: QuestStartStopEvent, running: boolean) {
        if (!e.quest) {
            return;
        }
        const questId = e.quest.getFormID();
        if (!this.questRuns.has(questId) && this.questRuns.size >= QUEST_RUNS_MAX) {
            const oldest = this.questRuns.keys().next();
            if (!oldest.done) {
                this.questRuns.delete(oldest.value);
            }
        }
        this.questRuns.delete(questId);
        this.questRuns.set(questId, running);
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

    // one name per engine command (Skyrim Platform matches the long or the
    // short name); the server's table knows both
    private static readonly serverOnlyCommands = [
        "setlevel", "advskill", "tim", "tgm", "tcl", "set",
        "save", "load", "savegame", "loadgame",
    ];

    // thuum ADR-068 rung 5, D-Q4 C: the quest commands a player may type,
    // each with the run words it may make here, as a static read of their
    // handlers has them (HYPOTHESIS until a trace): StartQuest starts a quest
    // with no story event; StopQuest stops it; SetStage starts a stopped one
    // with no story event, then sets the typed stage, marked beside the
    // start; ResetQuest resets the stages, then stops a running quest that
    // is not Start Game Enabled or starts a stopped one that is. The flag has
    // no read on this client (no Papyrus native or platform export names
    // TESQuest's kStartsEnabled, CommonLibSSE-NG include/RE/T/TESQuest.h:30),
    // so the run alone picks the reset's word, a running quest's stop or a
    // stopped one's start (a running quest that is Start Game Enabled still
    // marks a stop its reset does not make, a stopped one that is not a
    // start), and an unread run marks both. CompleteQuest is not
    // here: it flags the quest completed and never starts or stops it, so it
    // makes no run word or stage to mark
    private static readonly questCommands = new Map<string, number[]>([
        ["startquest", [QUEST_START]],
        ["stopquest", [QUEST_STOP]],
        ["setstage", [QUEST_START]],
        ["resetquest", [QUEST_STOP, QUEST_START]],
    ]);

    private readonly schemas: Map<CmdName, CmdArgument[]>;
    private readonly immuneSchema = ["mp", "kill", "resurrect", "moveto"];
    private readonly nonVanilaCommands = ["mp"];
    private questMarkMissingSaid = false;
    private readonly questRuns = new Map<number, boolean>();
}
