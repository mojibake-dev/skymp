#pragma once
#include "PlatformCall.h"

class VmCallback : public RE::BSScript::IStackCallbackFunctor
{
public:
  using OnResult = std::function<void(const Variable& result)>;

  static auto New(const OnResult& onResult_)
  {
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> res;
    res.reset(new VmCallback(onResult_));
    return res;
  }

  // the platform's mark for the script hooks (PlatformCall.h): a callback
  // that dies unrun (a stack the VM dropped) is forgotten here, the backstop
  ~VmCallback() override { PlatformCall::ForgetCallback(Base()); }

private:
  VmCallback(const OnResult& onResult_)
    : onResult(onResult_)
  {
  }

  // the pointer the dispatch received and the Stack holds (VmCall::Run notes
  // the same one)
  const RE::BSScript::IStackCallbackFunctor* Base() const
  {
    return static_cast<const RE::BSScript::IStackCallbackFunctor*>(this);
  }

  void operator()(Variable result) override
  {
    // the call's stack returned: the mark is spent before the result reaches
    // JS (PlatformCall.h)
    PlatformCall::ForgetCallback(Base());
    if (onResult)
      onResult(result);
  }

  bool CanSave() const override { return false; }

  void SetObject(
    const RE::BSTSmartPointer<RE::BSScript::Object>& object) override
  {
  }

  const OnResult onResult;
};
