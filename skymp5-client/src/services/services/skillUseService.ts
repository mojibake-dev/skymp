import { SkillUseEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { logTrace } from "../../logging";

// thuum docs/verbs/leveling.md (M5): skills rise by use, and the use is this
// game's engine's to see (R1) while the experience is the server's to compute
// (R0). The engine's one funnel, PlayerCharacter::UseSkill (the fork's hook,
// Skyrim Platform's skillUse event), fires once per landed hit, cast or craft
// with the skill, the action's base amount (an iron sword's 7, the weapon's
// base damage) and the source; its own write is nothing under the zeroed
// mults (DisableSkillAdvanceService). This service sums a second's amounts
// per skill (6 One-handed to 23 Enchanting; anything else dropped) and sends
// them as SkillUse (MsgType 63), each skill once, the amount bounded; the
// server runs the game's formulas (wire-rules leveling) into the record and
// answers with ActorValues when a level moves. One line per batch for the lab:
// "skill uses sent: 6:7.0 (1 use(s))".
export class SkillUseService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("skillUse", (e) => this.onSkillUse(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onSkillUse(e: SkillUseEvent) {
        if (e.skill < SkillUseService.firstSkill || e.skill > SkillUseService.lastSkill) {
            return;
        }
        if (!Number.isFinite(e.points) || e.points <= 0) {
            return;
        }
        this.pending.set(e.skill, (this.pending.get(e.skill) ?? 0) + e.points);
        this.events += 1;
    }

    private onUpdate() {
        const now = Date.now();
        if (now - this.lastSend < SkillUseService.batchMs || this.pending.size === 0) {
            return;
        }
        this.lastSend = now;
        const uses: { skill: number; amount: number }[] = [];
        this.pending.forEach((amount, skill) => {
            uses.push({ skill, amount: Math.min(amount, SkillUseService.maxAmount) });
        });
        this.pending.clear();
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.SkillUse, uses },
            reliability: "reliable",
        });
        logTrace(this, `skill uses sent: ${uses.map((u) => `${u.skill}:${u.amount.toFixed(1)}`).join(" ")} (${this.events} use(s))`);
        this.events = 0;
    }

    private readonly pending = new Map<number, number>();
    private events = 0;
    private lastSend = 0;
    private static readonly batchMs = 1000;
    // CommonLibSSE-NG include/RE/A/ActorValues.h: kOneHanded 6 to kEnchanting 23
    private static readonly firstSkill = 6;
    private static readonly lastSkill = 23;
    // the validator's bound on one amount (wire-validate SKILL_USE_MAX_AMOUNT)
    private static readonly maxAmount = 100_000;
}
