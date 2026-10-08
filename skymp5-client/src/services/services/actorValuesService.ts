import { MenuCloseEvent } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { ActorValuesMessage, ActorValueBase, SkillProgress, LegendarySkill } from "../messages/actorValuesMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// TESModPlatform.GetPlayerProgress' layout: xp, threshold, level, then per
// skill its level, xp and threshold, then per skill its legendary count
const SKILLS = 18;
const PROGRESS_LENGTH = 3 + SKILLS * 3 + SKILLS;

// thuum docs/verbs/actor-values.md: a player's actor values and progress
// follow its character. After a skill or a level increase, and when the
// level-up or the stats menu closes, the player's whole snapshot (its 164 base
// values, every skill's progress, its experience and level) is sent if it
// changed. The server's record, sent after a login or after a value it set,
// is applied through TESModPlatform's natives and counts as sent. The server
// holds its record until a report shows it applied, so a report from before
// is harmless.
export class ActorValuesService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("skillIncrease", () => this.report());
        this.controller.on("levelIncrease", () => this.report());
        this.controller.on("menuClose", (e) => this.onMenuClose(e));
        this.controller.emitter.on("actorValuesMessage", (e) => this.onActorValuesMessage(e));
    }

    // A loading screen closing also reports: after a login it is the first
    // moment the player's values are its own, so the server holds a record
    // of every player seconds into its session (the server's natives read
    // it), not only after the first skill increase. Unchanged values send
    // nothing (lastKey); a report before the login's record was applied is
    // refused by the server and harmless.
    private onMenuClose(e: MenuCloseEvent) {
        if (e.name === "LevelUp Menu" || e.name === "StatsMenu" || e.name === "Loading Menu") {
            this.report();
        }
    }

    private report() {
        const snapshot = this.read();
        if (!snapshot) {
            return;
        }
        const key = JSON.stringify(snapshot);
        if (key === this.lastKey) {
            return;
        }
        this.lastKey = key;
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.ActorValues, ...snapshot },
            reliability: "reliable",
        });
    }

    private onActorValuesMessage(e: ConnectionMessage<ActorValuesMessage>) {
        const m = e.message;
        try {
            const current = this.sp.callNative("TESModPlatform", "GetActorValueBases", undefined) as number[];
            m.bases.forEach((b) => {
                if (current[b.av] !== b.base) {
                    this.sp.callNative("TESModPlatform", "SetActorValueBase", undefined, b.av, b.base);
                }
            });
            const progress = new Array<number>(PROGRESS_LENGTH).fill(0);
            progress[0] = m.xp;
            progress[1] = m.threshold;
            progress[2] = m.level;
            const read = this.sp.callNative("TESModPlatform", "GetPlayerProgress", undefined) as number[];
            for (let i = 0; i < SKILLS; ++i) {
                // a skill the record does not name keeps the engine's own
                const own = m.skills.find((s) => s.skill === i);
                progress[3 + i * 3] = own ? own.level : read[3 + i * 3];
                progress[3 + i * 3 + 1] = own ? own.xp : read[3 + i * 3 + 1];
                progress[3 + i * 3 + 2] = own ? own.threshold : read[3 + i * 3 + 2];
                const legendary = m.legendary.find((l) => l.skill === i);
                progress[3 + SKILLS * 3 + i] = legendary ? legendary.count : 0;
            }
            if (this.sp.callNative("TESModPlatform", "SetPlayerProgress", undefined, progress) !== true) {
                logError(this, "SetPlayerProgress refused the server's record");
            }
            logTrace(this, "applied the server's actor values: level", m.level, "bases", m.bases.length);
        } catch (err) {
            logError(this, "applying the server's actor values failed", err);
            return;
        }
        // the record counts as sent: it is not echoed back
        const applied = this.read();
        this.lastKey = applied ? JSON.stringify(applied) : this.lastKey;
    }

    private read(): Omit<ActorValuesMessage, "t"> | undefined {
        try {
            const bases = this.sp.callNative("TESModPlatform", "GetActorValueBases", undefined) as number[];
            const p = this.sp.callNative("TESModPlatform", "GetPlayerProgress", undefined) as number[];
            if (!bases || bases.length === 0 || !p || p.length !== PROGRESS_LENGTH) {
                return undefined;
            }
            const out: ActorValueBase[] = bases.map((base, av) => ({ av, base }));
            const skills: SkillProgress[] = [];
            const legendary: LegendarySkill[] = [];
            for (let i = 0; i < SKILLS; ++i) {
                skills.push({ skill: i, level: p[3 + i * 3], xp: p[3 + i * 3 + 1], threshold: p[3 + i * 3 + 2] });
                const count = p[3 + SKILLS * 3 + i];
                if (count > 0) {
                    legendary.push({ skill: i, count });
                }
            }
            return { bases: out, skills, xp: p[0], threshold: p[1], level: p[2], legendary };
        } catch (err) {
            logError(this, "reading the player's actor values failed", err);
            return undefined;
        }
    }

    private lastKey = "";
}
