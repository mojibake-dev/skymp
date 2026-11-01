#pragma once
#include "IPapyrusClass.h"

class MpObjectReference;
struct LocationalData;
namespace espm {
struct LookupResult;
}

class PapyrusObjectReference final
  : public IPapyrusClass<PapyrusObjectReference>
{
public:
  const char* GetName() override { return "ObjectReference"; }

  // The one creation path for a server-placed form (PlaceAtMe's core): the
  // reference or actor built at `where` with the anchor's callbacks, fated
  // when an NPC (ADR-050 decision 3), its create word sent from `where`.
  // MpActor::DropItem places the dropped copy through it at the drop point
  // directly, because a copy created at the anchor and moved after is created
  // on the clients where the create word said (thuum docs/verbs/
  // prop-physics.md, x-prop-launch v3 190643: the copy spawned inside the
  // dropper's capsule and was ejected at the first Havok step).
  static MpObjectReference& PlaceAt(MpObjectReference& anchor,
                                    const espm::LookupResult& form,
                                    const LocationalData& where);

  VarValue IsHarvested(VarValue self, const std::vector<VarValue>& arguments);
  VarValue IsDisabled(VarValue self, const std::vector<VarValue>& arguments);
  VarValue IsDeleted(VarValue self, const std::vector<VarValue>& arguments);
  VarValue GetScale(VarValue self, const std::vector<VarValue>& arguments);
  VarValue SetScale(VarValue self, const std::vector<VarValue>& arguments);
  VarValue EnableNoWait(VarValue self, const std::vector<VarValue>& arguments);
  VarValue DisableNoWait(VarValue self,
                         const std::vector<VarValue>& arguments);
  VarValue AddItem(VarValue self, const std::vector<VarValue>& arguments);
  VarValue RemoveItem(VarValue self, const std::vector<VarValue>& arguments);
  VarValue RemoveAllItems(VarValue self,
                          const std::vector<VarValue>& arguments);
  VarValue GetItemCount(VarValue self, const std::vector<VarValue>& arguments);
  VarValue GetAnimationVariableBool(VarValue self,
                                    const std::vector<VarValue>& arguments);
  VarValue PlaceAtMe(VarValue self, const std::vector<VarValue>& arguments);
  VarValue SetAngle(VarValue self, const std::vector<VarValue>& arguments);
  VarValue Enable(VarValue self, const std::vector<VarValue>& arguments);
  VarValue Disable(VarValue self, const std::vector<VarValue>& arguments);
  VarValue Delete(VarValue self, const std::vector<VarValue>& arguments);
  VarValue BlockActivation(VarValue self,
                           const std::vector<VarValue>& arguments);
  VarValue IsActivationBlocked(VarValue self,
                               const std::vector<VarValue>& arguments);
  VarValue Activate(VarValue self, const std::vector<VarValue>& arguments);
  VarValue GetPositionX(VarValue self, const std::vector<VarValue>& arguments);
  VarValue GetPositionY(VarValue self, const std::vector<VarValue>& arguments);
  VarValue GetPositionZ(VarValue self, const std::vector<VarValue>& arguments);
  VarValue SetPosition(VarValue self, const std::vector<VarValue>& arguments);
  VarValue GetTotalItemWeight(VarValue self,
                              const std::vector<VarValue>& arguments);

  VarValue GetBaseObject(VarValue self,
                         const std::vector<VarValue>& arguments);
  VarValue PlayAnimation(VarValue self,
                         const std::vector<VarValue>& arguments);
  VarValue PlayAnimationAndWait(VarValue self,
                                const std::vector<VarValue>& arguments);
  VarValue PlayGamebryoAnimation(VarValue self,
                                 const std::vector<VarValue>& arguments);
  VarValue MoveTo(VarValue self, const std::vector<VarValue>& arguments);

  VarValue SetOpen(VarValue self, const std::vector<VarValue>& arguments);

  VarValue Is3DLoaded(VarValue self, const std::vector<VarValue>& arguments);

  VarValue GetLinkedRef(VarValue self, const std::vector<VarValue>& arguments);

  VarValue GetNthLinkedRef(VarValue self,
                           const std::vector<VarValue>& arguments);

  VarValue GetParentCell(VarValue self,
                         const std::vector<VarValue>& arguments);

  VarValue GetOpenState(VarValue self, const std::vector<VarValue>& arguments);

  VarValue GetAllItemsCount(VarValue self,
                            const std::vector<VarValue>& arguments);

  VarValue IsContainerEmpty(VarValue self,
                            const std::vector<VarValue>& arguments);

  static constexpr auto kOriginalNameExpression = R"(%original_name%)";

  // %original_name% will be replaced with the original localized name
  // client-side
  VarValue SetDisplayName(VarValue self,
                          const std::vector<VarValue>& arguments);

  VarValue GetDistance(VarValue self, const std::vector<VarValue>& arguments);

  void Register(VirtualMachine& vm,
                std::shared_ptr<IPapyrusCompatibilityPolicy> policy) override;
};
