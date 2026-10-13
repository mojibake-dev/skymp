import { Actor } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";

export class RagdollService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.once("update", () => this.onceUpdate());
    }

    // TODO: think about tracking ragdoll state of player
    public safeRemoveRagdollFromWorld = (
        actor: Actor,
        afterRemoveCallback: () => void
    ) => {
        this.setLocalDamageMult(0);
        actor.forceRemoveRagdollFromWorld().then(() => {
            this.controller.once("update", () => {
                this.setLocalDamageMult(this.defaultLocalDamageMult);
                afterRemoveCallback();
            });
        });
    };

    private onceUpdate() {
        this.setLocalDamageMult(this.defaultLocalDamageMult);
    }

    private setLocalDamageMult(damageMult: number) {
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCE", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCH", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCL", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCN", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCVE", damageMult);
        this.sp.Game.setGameSettingFloat("fDiffMultHPToPCVH", damageMult);
    }

    // thuum ADR-028: the server computes all damage one actor deals another,
    // so a hit from any actor counts nothing to the player here; the
    // server's ChangeValues carry it. At 1, a figure's attack or spell that
    // landed in this game hurt the player locally, and the client's health
    // report carried that to the server as world damage on top of the
    // server's own count (x-uf-ethereal 20261013-082909: Unrelenting Force
    // took 50, the server's hit 10; docs/verbs/shouts.md). The world's
    // damage (a fall) goes through no actor's multiplier and stays the
    // game's own
    private readonly defaultLocalDamageMult = 0;
}
