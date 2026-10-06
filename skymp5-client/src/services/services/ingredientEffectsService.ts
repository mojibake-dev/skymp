import { ClientListener, CombinedController, Sp } from "./clientListener";
import { EquipEvent, FormType } from "skyrimPlatform";
import { MsgType } from "../../messages";

// thuum docs/verbs/learned-effects.md: after the player eats an ingredient,
// report the effects its engine now knows of it; the server keeps the report
// after the eat it saw (sendInputsService's OnEquip) and teaches the effects
// again after every login. Whether the engine learns before or after the
// equip event is not known, so the mask is read on each update for two
// seconds and sent whenever it grew past what was last sent.
export class IngredientEffectsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("equip", (e) => this.onEquip(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onEquip(e: EquipEvent) {
        if (!e.actor || !e.baseObj) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (!player || e.actor.getFormID() !== player.getFormID()) {
            return;
        }
        if (e.baseObj.getType() !== FormType.Ingredient) {
            return;
        }
        this.watching.set(e.baseObj.getFormID(), Date.now() + 2000);
    }

    private onUpdate() {
        if (this.watching.size === 0) {
            return;
        }
        const now = Date.now();
        for (const [id, until] of this.watching) {
            const ingredient = this.sp.Ingredient.from(this.sp.Game.getFormEx(id));
            if (!ingredient) {
                this.watching.delete(id);
                continue;
            }
            let mask = 0;
            for (let i = 0; i < 4; ++i) {
                if (ingredient.getIsNthEffectKnown(i)) {
                    mask |= 1 << i;
                }
            }
            const sent = this.sent.get(id) ?? 0;
            if ((mask | sent) !== sent) {
                this.sent.set(id, mask | sent);
                this.controller.emitter.emit("sendMessage", {
                    message: { t: MsgType.IngredientEffectsKnown, ingredient: id, mask },
                    reliability: "reliable",
                });
            }
            if (now >= until) {
                this.watching.delete(id);
            }
        }
    }

    // ingredient form id -> watch until (ms since epoch)
    private watching = new Map<number, number>();
    // ingredient form id -> mask last sent
    private sent = new Map<number, number>();
}
