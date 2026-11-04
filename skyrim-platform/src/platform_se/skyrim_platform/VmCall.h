#pragma once
#include "PlatformCall.h"
#include "VmCallback.h"
#include "VmFunctionArguments.h"

class VmCall
{
public:
  static bool Run(IVM& vm, FixedString className, FixedString functionName,
                  const RE::BSTSmartPointer<RE::BSScript::Object>* self,
                  const VmFunctionArguments& arguments,
                  const VmCallback::OnResult& onResult,
                  std::function<const char*()> getExceptionInfo)
  {
    auto args = const_cast<VmFunctionArguments*>(&arguments);
    auto functor = VmCallback::New(onResult);
    // the platform's latent call, marked for the script hooks by its
    // callback (PlatformCall.h; ScriptNatives.cpp's hook on the VM's
    // dispatch reads the Stack's callback against the live set): noted
    // before the dispatch, since the VM may run the stack before the
    // dispatcher returns; forgotten here when the dispatch is refused, else
    // when the callback runs or dies (VmCallback.h)
    PlatformCall::NoteCallback(functor.get());
    if (self && *self) {
      const bool dispatched = vm.DispatchMethodCall2(
        (*self)->handle, className, functionName, args, functor);
      if (!dispatched) {
        PlatformCall::ForgetCallback(functor.get());
      }
      return dispatched;
    }

    std::stringstream err;

    bool res =
      ([](IVM& vm, FixedString& className, FixedString& functionName,
          VmFunctionArguments* args,
          RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>& functor,
          std::stringstream& err,
          std::function<const char*()>& getExceptionInfo) {
        __try {
          return vm.DispatchStaticCall(className, functionName, args, functor);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          if (getExceptionInfo)
            err << getExceptionInfo();
          return false;
        }
      })(vm, className, functionName, args, functor, err, getExceptionInfo);

    if (!res) {
      PlatformCall::ForgetCallback(functor.get());
    }

    if (!err.str().empty())
      throw std::runtime_error("VmCall::Run " + err.str());

    return res;
  }
};
