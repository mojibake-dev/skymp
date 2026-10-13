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
        controller.on('update', () => { this.frame++; });
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
              this.dispelFigureSpellOnPlayer(e);
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

        // thuum docs/verbs/spell-cast.md: a magic hit comes twice in a frame
        // (0 to 2 ms apart, x-dual-cast), so every other copy of one caster,
        // target and spell within a frame is dropped: one hand's hit counts
        // once, two hands' in one frame (both buttons pressed together)
        // twice. The filter here before dropped any magic hit within 100 ms
        // of the caster's last, the other hand's with it, and the server
        // counted a two-handed stream once (x-dual-cast, x-dual-stack: the
        // game runs both). A double that slips through adds nothing: the
        // server counts a stream by its time, not its reports
        if (isSpell || isScroll) {
            if (this.magicHitsFrame !== this.frame) {
                this.magicHitsFrame = this.frame;
                this.magicHitsThisFrame.clear();
            }
            const key = `${e.aggressor.getFormID()}:${e.target.getFormID()}:${e.source.getFormID()}`;
            const n = (this.magicHitsThisFrame.get(key) || 0) + 1;
            this.magicHitsThisFrame.set(key, n);
            if (n % 2 === 0) {
                return;
            }
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

    // thuum docs/verbs/shouts.md: a figure's relayed cast is drawn in this game
    // and its projectile lands here as any spell's would, but what it does to
    // this game's player is the server's to say (its ChangeValues and
    // MagicEffects): the spell's effects leave the player at once. A relayed
    // Fire Breath burned test 2's own game to 0.34 while the server counted
    // half (x-shout-relay 20261013-082608); an instant hit is overwritten by
    // the server's value anyway, an effect over time was not
    private dispelFigureSpellOnPlayer(e: HitEvent) {
        if (!e.target || e.target.getFormID() !== 0x14) {
            return;
        }
        const spell = this.sp.Spell.from(e.source);
        const player = this.sp.Game.getPlayer();
        if (spell && player) {
            player.dispelSpell(spell);
        }
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

    private frame = 0;
    private magicHitsFrame = -1;
    private magicHitsThisFrame = new Map<string, number>();
}
