#pragma once
#include <cstdint>

class MpActor;
struct ConditionEvaluatorContext;

class ConditionFunction
{
public:
  virtual ~ConditionFunction() = default;

  virtual const char* GetName() const = 0;

  // https://en.uesp.net/wiki/Skyrim_Mod:Function_Indices
  virtual uint16_t GetFunctionIndex() const = 0;

  virtual float Execute(MpActor& actor, uint32_t parameter1,
                        uint32_t parameter2,
                        const ConditionEvaluatorContext& context) = 0;

  // Whether parameter `index` (1 or 2) is a form id. A condition record
  // stores a form parameter in the file's own index space (Hailstone's
  // HasMagicEffect names its MGEF as 0x030B2C7F, Apocalypse's third
  // master byte; read on sky-srv 2026-10-20), so the reader maps it to the
  // global id through the owning record before any function compares it
  // (MpActor::EffectConditionsHold). Functions that take no form leave it
  // false; the older ones compare their parameter raw, which holds only
  // for Skyrim.esm's forms (thuum docs/verbs/spell-cast.md, train-30).
  virtual bool IsFormParameter([[maybe_unused]] int index) const
  {
    return false;
  }
};
