#pragma once
#include "FormDesc.h"
#include "NiPoint3.h"
#include "PartOne.h"
#include <cstdint>
#include <string>
#include <vector>

class MpActor;

namespace MovementValidation {
// `placed`, when given, is set when the move passes as a jump the server
// vouches for (an arrival after its own teleport, a cast teleport's permitted
// landing, a permitted console jump, or a step no run makes that the ground
// budget takes within the cast jump that waits: a short teleport's landing, a
// bond's swap, docs/verbs/shadowbond.md): the other games place the figure at
// this report instead of gliding it (thuum docs/verbs/cast-teleport.md).
// `hostedCellChange`: the report moves a hosted actor into another cell under
// the cell-change rule (thuum docs/verbs/cell-host.md, judged by the caller),
// so the same-cell and distance bounds below stand aside for it alone
bool Validate(PartOne& partOne, const NiPoint3& currentPos,
              const NiPoint3& currentRot, const FormDesc& currentCellOrWorld,
              const NiPoint3& newPos, const FormDesc& newCellOrWorld,
              Networking::UserId userId, MpActor* actor,
              const std::vector<std::string>& espmFiles,
              bool* placed = nullptr, bool hostedCellChange = false);
}
