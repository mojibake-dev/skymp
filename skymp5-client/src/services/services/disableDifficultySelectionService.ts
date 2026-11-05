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

    // thuum D-T5 (Eli, 2026-11-05: "difficulty should apply to all things";
    // "at testing everything should be set to adept"): the difficulty this
    // client pins, from the client setting difficulty, an integer from 0
    // (Novice) to 5 (Legendary), so the pin moves with a settings line instead
    // of a build; Adept (2) when the setting is absent or refused (logged once,
    // read once here), not upstream's 5, until the server applies the one
    // difficulty to every hit a player takes or deals (D-T5 C; at Adept both
    // tables read 1, so traps and bandits agree meanwhile). This writes the
    // INI setting alone, and that never reaches the player's own difficulty
    // field, which DamageActor indexes (docs/research/re-37335-and-rune-
    // landing.md:30; CommonLib PlayerCharacter.h:399, GetGameStatsData's
    // difficulty): CONFIRMED by run, x-trap-hit-difficulty 20261105-205713, a
    // spike wall's hit cost 59 at the pin's 5 and at a laid 2 alike, the six
    // fDiffMultHPToPC* at the engine's own table, so the difficulty in force is
    // the seat's own (Adept on the lab's seats) and upstream's pin never pinned
    // the damage; D-T5 C writes that field from the server's one value. The
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

    private static readonly defaultDifficulty = 2;

    private readonly difficulty: number;

    private counter = 0;
}
