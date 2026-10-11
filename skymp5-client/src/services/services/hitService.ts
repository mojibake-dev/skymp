// TODO: refactor this out
import { localIdToRemoteId } from "../../view/worldViewMisc";

import { FormType, HitEvent, storage } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { Hit } from "../messages/hitMessage";
import { shoutGate } from "./shoutService";

export class HitService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on('hit', (e) => this.onHit(e));
    }

    private onHit(e: HitEvent) {
        // TODO: add more logging in case of 'return'
        // TODO: allow non-weapon sources
        const aggressor = e.aggressor.getFormID();
        if (aggressor < 0xff000000 && aggressor !== 0x14) return; // all skymp npcs are FF+

        if (aggressor >= 0xff000000) {
            // TODO: make host service
            const hosted = storage['hosted'];
            let alreadyHosted = false;
            if (Array.isArray(hosted)) {
                const remoteId = localIdToRemoteId(aggressor);
                if (hosted.includes(remoteId) || hosted.includes(remoteId + 0x100000000)) {
                    alreadyHosted = true;
                }
            }

            if (!alreadyHosted) {
              return;
            }
        }

        const base = e.target.getBaseObject();
        const type = base?.getType();

        if (type === FormType.Static || type === FormType.MovableStatic) {
            return;
        }

        const isWeapon = this.sp.Weapon.from(e.source);
        const isSpell = !isWeapon && this.sp.Spell.from(e.source);
        const isScroll = !isWeapon && !isSpell && this.sp.Scroll.from(e.source);

        if (!isWeapon && !isSpell && !isScroll) {
            return;
        }

        // prevent double hit that happens for some reason with magic projectiles
        if (isSpell || isScroll) {
            const aggressorId = e.aggressor.getFormID();
            const now = Date.now();

            const lastHitTime = this.recentMagicHits.get(aggressorId);
            if (lastHitTime && now - lastHitTime < 100) {
                return;
            }

            this.recentMagicHits.set(aggressorId, now);
        }

        const data = this.getHitData(e);
        const send = () => this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.OnHit, data },
            reliability: "reliable"
        });

        // thuum docs/verbs/shouts.md: the player's spell hits after a
        // shout's release wait for the shout's report (shoutService)
        if (isSpell && aggressor === 0x14 && shoutGate.pending) {
            shoutGate.held.push(send);
            return;
        }
        send();
    }

    private getHitData(e: HitEvent): Hit {
        const hitData: Hit = {
            aggressor: localIdToRemoteId(e.aggressor.getFormID()),
            isBashAttack: e.isBashAttack,
            isHitBlocked: e.isHitBlocked,
            isPowerAttack: e.isPowerAttack,
            isSneakAttack: e.isSneakAttack,
            projectile: e.projectile ? e.projectile.getFormID() : 0,
            source: e.source ? e.source.getFormID() : 0,
            target: localIdToRemoteId(e.target.getFormID())
        }
        return hitData;
    }

    private recentMagicHits: Map<number, number> = new Map();
}
