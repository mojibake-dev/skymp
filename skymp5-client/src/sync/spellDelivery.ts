import { Spell } from "skyrimPlatform";

// thuum docs/verbs/spell-cast.md: a spell whose effects are all delivered
// to their target, by touch or at a target actor, so no aim decides where
// they land (a cosmetic projectile, Azure Reconstruction's, included;
// MagicEffect.GetDeliveryType, SKSE; CommonLibSSE-NG
// include/RE/M/MagicSystem.h:39-45: kTouch 1, kTargetActor 3). The sender
// names such a cast's target (magicSyncService) and the receiver casts it at
// its own player when named (remoteServer): one reading of the delivery.
export function landsOnItsTarget(spell: Spell): boolean {
    const n = spell.getNumEffects();
    for (let i = 0; i < n; ++i) {
        const delivery = spell.getNthEffectMagicEffect(i)?.getDeliveryType();
        if (delivery !== 1 && delivery !== 3) {
            return false;
        }
    }
    return n > 0;
}
