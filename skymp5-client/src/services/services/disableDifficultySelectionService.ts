import { ClientListener, Sp, CombinedController } from "./clientListener";
import { logError } from "../../logging";

export class DisableDifficultySelectionService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.difficulty = this.readDifficulty();
        this.controller.on("update", () => this.onUpdate());
    }

    private onUpdate() {
        this.counter++;
        if (this.counter >= 60) {
            this.counter = 0;
            this.sp.Utility.setINIInt("iDifficulty:GamePlay", this.difficulty);
        }
    }

    // thuum D-T5 (built ahead of Eli's word, so it stages and does not merge
    // without it): the difficulty this client pins, from the client setting
    // difficulty, an integer from 0 (Novice) to 5 (Legendary), so the pin
    // moves with a settings line instead of a build; upstream's 5 when the
    // setting is absent or refused (logged once, read once here). Since D-T4 B
    // the world's damage to the player is meant to follow it (RagdollService:
    // the engine's own fDiffMultHPToPC* for the difficulty in force); HYPOTHESIS:
    // this writes the INI setting, while the static read has DamageActor index
    // the player's own difficulty field (docs/research/re-37335-and-rune-
    // landing.md:30; CommonLib PlayerCharacter.h:399), and whether the write
    // reaches that field is x-trap-hit-difficulty's two legs (5 and 2). The
    // damage one actor deals another is the server's and takes no difficulty
    // yet (TES5DamageFormula.cpp's TODO(#461))
    private readDifficulty(): number {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        const v = s ? s["difficulty"] : undefined;
        if (v === undefined) {
            return DisableDifficultySelectionService.defaultDifficulty;
        }
        if (typeof v === "number" && Number.isInteger(v) && v >= 0 && v <= 5) {
            return v;
        }
        logError(this, `difficulty ${JSON.stringify(v)} refused (an integer from 0 to 5); ${DisableDifficultySelectionService.defaultDifficulty} instead`);
        return DisableDifficultySelectionService.defaultDifficulty;
    }

    private static readonly defaultDifficulty = 5;

    private readonly difficulty: number;

    private counter = 0;
}
