#pragma once
#include "WorldState.h"
float CropRegeneration(float newAttributeValue, float secondsAfterLastRegen,
                       float attributeRate, float attributeRateMult,
                       float oldAttributeValue, bool hasActiveMagicEffects);

// The regeneration rate (percent of the maximum a second) and multiplier
// (percent) the crop judges an attribute by: the larger of the race's and the
// actor's, the actor's alone for stamina while blocking. The rest rule
// (thuum docs/verbs/rest.md) regenerates at the same rates.
struct RegenRate
{
  float rate = 0.f;
  float rateMult = 0.f;
};

RegenRate GetHealthRegenRate(MpActor* actor);
RegenRate GetMagickaRegenRate(MpActor* actor);
RegenRate GetStaminaRegenRate(MpActor* actor);

float CropHealthRegeneration(float newAttributeValue,
                             float secondsAfterLastRegen, MpActor* actor);

float CropMagickaRegeneration(float newAttributeValue,
                              float secondsAfterLastRegen, MpActor* actor);

float CropStaminaRegeneration(float newAttributeValue,
                              float secondsAfterLastRegen, MpActor* actor);

float CropPeriodAfterLastRegen(float secondsAfterLastRegen,
                               float maxValidPeriod = 2.0f,
                               float defaultPeriod = 1.0f);

float CropValue(float value, float min = 0.f, float max = 1.0f);
