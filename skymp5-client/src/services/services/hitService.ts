// TODO: refactor this out
import { localIdToRemoteId } from "../../view/worldViewMisc";

import { ActiveMagicEffect, Form, FormType, Game, HitEvent, MagicEffect, ObjectReference, Spell, storage } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { Hit } from "../messages/hitMessage";
import { shoutGate } from "./shoutService";

export class HitService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on('hit', (e) => this.onHit(e));
        // thuum docs/verbs/spell-cast.md (the class pass, x-class-self
        // 20261018-011427 and x-class-streams 013048): a spell whose effects
        // land through an area (a Self burst, an area stream's splash, a
        // target-actor spell) applies them through AddTarget, which raises
        // no hit event for the caster's game to send; the explosion's own
        // hit event carries its weapon, Unarmed (0x1F4) when it has none, and
        // the server counted a punch (3.71 damage) and no spell at all. So
        // the player's spell effects landing on others are reported from
        // the apply, as hits of the spell in hand that carries the effect,
        // once per target and spell within a frame beside any hit event
        controller.on('magicEffectApply', (e) => this.onEffectApplied(e));
        controller.on('update', () => {
            this.frame++;
            this.sendDue();
            this.sendHeldUnarmed();
        });
    }

    private onEffectApplied(e: { activeEffect: ActiveMagicEffect; effect: MagicEffect; spell?: Form | null; caster: ObjectReference; target: ObjectReference }) {
        if (!e.caster || !e.target || !e.effect || e.caster.getFormID() !== 0x14) {
            return;
        }
        const targetId = e.target.getFormID();
        if (targetId < 0xff000000) {
            return; // another player's figure, never the player itself or a world actor
        }
        this.appliedOn.set(targetId, this.frame);
        // the applying spell as the native side read it from the active
        // effect (a spell the player never equipped included: Core Impact
        // Framework's kill spell, docs/verbs/headshot.md); the hands' spells
        // when it gave none
        const spell = e.spell ? e.spell.getFormID() : this.spellInHandWith(e.effect.getFormID());
        if (!spell) {
            return;
        }
        const key = `${targetId}:${spell}`;
        const sentAt = this.magicSent.get(key);
        if (sentAt !== undefined && this.frame - sentAt <= HitService.dedupFrames) {
            return;
        }
        this.magicSent.set(key, this.frame);
        const data: Hit = {
            aggressor: localIdToRemoteId(0x14),
            isBashAttack: false,
            isHitBlocked: false,
            isPowerAttack: false,
            isSneakAttack: false,
            projectile: 0,
            source: spell,
            target: localIdToRemoteId(targetId)
        };
        this.sendSpellHit(spell, data);
    }

    // the player's equipped spell (left, right, voice, instant) whose
    // effects include the magic effect, or 0
    private spellInHandWith(effectId: number): number {
        const player = Game.getPlayer();
        if (!player) {
            return 0;
        }
        for (let slot = 0; slot < 4; ++slot) {
            const spell: Spell | null = player.getEquippedSpell(slot);
            if (!spell) {
                continue;
            }
            const n = spell.getNumEffects();
            for (let i = 0; i < n; ++i) {
                const effect = spell.getNthEffectMagicEffect(i);
                if (effect && effect.getFormID() === effectId) {
                    return spell.getFormID();
                }
            }
        }
        return 0;
    }

    // the player's spell hit: behind a pending shout's report, then two
    // updates behind its cast (self-cast.md)
    private sendSpellHit(spell: number, data: Hit) {
        const send = () => this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.OnHit, data },
            reliability: "reliable"
        });
        if (shoutGate.pending) {
            shoutGate.held.push({ spell, send });
            return;
        }
        shoutGate.reportFromHit(spell, "no release seen");
        this.due.push({ frame: this.frame + HitService.spellHitDelayUpdates, send });
    }

    // an unarmed hit held one update: an explosion's hit event names
    // Unarmed as its weapon, and a player casting a burst or an area spell
    // is no puncher, so one that lands beside the player's own effect on the
    // same target in the same frame or the next is the explosion's and goes
    // nowhere (x-class-self: source 0x1F4 counted as 3.71 of unarmed damage
    // at every Shock Nova and Battletide)
    private sendHeldUnarmed() {
        const keep: Array<{ frame: number; target: number; send: () => void }> = [];
        for (const held of this.heldUnarmed) {
            const applied = this.appliedOn.get(held.target);
            if (applied !== undefined && Math.abs(applied - held.frame) <= 1) {
                continue;
            }
            if (this.frame > held.frame) {
                held.send();
            } else {
                keep.push(held);
            }
        }
        this.heldUnarmed = keep;
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

            // a figure's own hit is its player's to report; what its spell
            // does to this game's player is the server's number and the
            // engine's everything else (Skyrim Platform's PlayerAddTarget
            // hook, Hooks.cpp; thuum docs/verbs/push.md)
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
        // shout's release wait for the shout's report (shoutService); one
        // of the equipped shout's spells with no report of it, the release
        // unseen, reports it first. thuum docs/verbs/self-cast.md (playtest
        // twenty-one: Battletide's area hit reached the server in the same
        // millisecond as its cast, ahead of it, and was refused,
        // E_SPELL_NO_CAST): the player's own spell hit goes two updates
        // later, behind any cast this frame or the next sends (one reliable
        // channel keeps the order to the server). A hit the apply already
        // reported for this target and spell within a frame is one report
        if (isSpell && aggressor === 0x14) {
            const key = `${e.target.getFormID()}:${e.source.getFormID()}`;
            const sentAt = this.magicSent.get(key);
            if (sentAt !== undefined && this.frame - sentAt <= HitService.dedupFrames) {
                return;
            }
            this.magicSent.set(key, this.frame);
            this.sendSpellHit(e.source.getFormID(), data);
            return;
        }
        if (isWeapon && aggressor === 0x14 && e.source.getFormID() === HitService.kUnarmed) {
            this.heldUnarmed.push({ frame: this.frame, target: e.target.getFormID(), send });
            return;
        }
        send();
    }

    private sendDue() {
        while (this.due.length > 0 && this.due[0].frame <= this.frame) {
            const next = this.due.shift();
            next?.send();
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
    private due: Array<{ frame: number; send: () => void }> = [];
    private static readonly spellHitDelayUpdates = 2;
    // Skyrim.esm's Unarmed weapon, the explosion's stand-in for a weapon
    private static readonly kUnarmed = 0x1f4;
    // a hit event and an apply of the same spell on the same target this
    // many frames apart are one hit
    private static readonly dedupFrames = 2;
    private magicSent = new Map<string, number>();
    private appliedOn = new Map<number, number>();
    private heldUnarmed: Array<{ frame: number; target: number; send: () => void }> = [];
    private magicHitsFrame = -1;
    private magicHitsThisFrame = new Map<string, number>();
}
