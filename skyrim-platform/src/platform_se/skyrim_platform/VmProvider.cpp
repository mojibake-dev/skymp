#include "VmProvider.h"
#include "GetNativeFunctionAddr.h"
#include "NullPointerException.h"
#include <optional>

namespace {
const RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo>& FindType(
  const std::string& className)
{
  auto vm = VM::GetSingleton();
  if (!vm)
    throw NullPointerException("vm");

  for (int i = 0; i < 2; ++i) {
    for (auto& [thisClassName, classInfo] : vm->objectTypeMap) {
      if (!stricmp(thisClassName.data(), className.data()) != 0)
        return classInfo;
    }
    if (!vm->ReloadType(className.data()))
      break;
  }

  static const RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> notFound;
  return notFound;
}

struct AdditionalFunctionInfo
{
  enum class Type
  {
    Invalid = -1,
    Global,
    Member
  };
  Type type = Type::Invalid;

  std::vector<TypeInfo> paramTypes;
};

using FunctionFindResult =
  std::pair<RE::BSTSmartPointer<RE::BSScript::IFunction>,
            AdditionalFunctionInfo>;

struct FindFunctionCache
{
  std::unordered_map<
    std::string,
    std::unordered_map<std::string, std::unique_ptr<FunctionInfo_>>>
    funcByFullName;
};

FunctionFindResult FindFunction(const RE::BSScript::ObjectTypeInfo* classInfo,
                                const std::string& funcName)
{
  FunctionFindResult res;

  for (uint32_t i = 0; i < classInfo->GetNumGlobalFuncs(); ++i) {
    auto& f = classInfo->GetGlobalFuncIter()[i].func;
    if (f && !stricmp(f->GetName().data(), funcName.data())) {
      res.first = f;
      res.second.type = AdditionalFunctionInfo::Type::Global;
      break;
    }
  }
  for (uint32_t i = 0; i < classInfo->GetNumMemberFuncs(); ++i) {
    auto& f = classInfo->GetMemberFuncIter()[i].func;
    if (f && !stricmp(f->GetName().data(), funcName.data())) {
      res.first = f;
      res.second.type = AdditionalFunctionInfo::Type::Member;
      break;
    }
  }

  if (res.first) {
    FixedString outNameDummy;
    auto n = res.first->GetParamCount();
    res.second.paramTypes.resize(n);
    for (uint32_t i = 0; i < n; ++i)
      res.first->GetParam(i, outNameDummy, res.second.paramTypes[i]);
  }

  return res;
}

class VmFunctionInfo : public FunctionInfo_
{
public:
  VmFunctionInfo(const FunctionFindResult& r_)
    : f(r_.first)
    , info(r_.second)
  {
  }

  size_t GetParamCount() override { return info.paramTypes.size(); }

  ValueType GetReturnType() override
  {
    return MakeValueType(f->GetReturnType());
  }

  bool IsGlobal() override
  {
    switch (info.type) {
      case AdditionalFunctionInfo::Type::Global:
        return true;
      case AdditionalFunctionInfo::Type::Member:
        return false;
      default:
        throw std::runtime_error("Invalid function type");
    }
  }

  bool IsLatent() override { return GetResult().isLatent; }

  bool IsNative() override { return GetResult().isNative; }

  RE::BSTSmartPointer<RE::BSScript::IFunction> GetIFunction() override
  {
    return f;
  }

  bool UsesLongSignature() override { return GetResult().useLongSignature; }

  ValueType GetParamType(size_t i) override
  {
    if (i >= info.paramTypes.size())
      throw std::runtime_error("GetParamType: Bad index (" +
                               std::to_string(i) + "), size was (" +
                               std::to_string(info.paramTypes.size()) + ")");
    return MakeValueType(info.paramTypes[i]);
  }

private:
  std::optional<GetNativeFunctionAddr::Result> result;
  RE::BSTSmartPointer<RE::BSScript::IFunction> f;
  AdditionalFunctionInfo info;

  static ValueType MakeValueType(TypeInfo typeInfo)
  {
    ValueType res;
    res.type = typeInfo.GetUnmangledRawType();
    // An object's raw type is its class pointer; an object array's is that
    // pointer with its first bit set (CommonLibSSE-NG TypeInfo.h, the RawType
    // comment), so the element class comes from the same bits and Sp3Api
    // stamps it on the array for sp3.js to wrap each element. With the name
    // left empty, every array a native returned threw in wrapObject
    // (ActorValueInfo.getPerks, thuum docs/verbs/perks.md).
    // TypeInfo::GetTypeInfo masks kObjectArray (0xB) off an array's raw
    // type, which also clears a pointer's bit 3 and sent an 8-byte-aligned
    // class to a wrong address (two launch crashes on 2b8c7592, 2026-11-01):
    // the one bit is masked here instead. A raw type below kArraysEnd (a
    // literal kObjectArray, an array of none) names no class and keeps the
    // empty name, as every array did before.
    if (typeInfo.IsObject() || typeInfo.IsObjectArray()) {
      const auto raw = static_cast<std::size_t>(typeInfo.GetRawType());
      auto objTypeInfo = reinterpret_cast<decltype(typeInfo.GetTypeInfo())>(
        raw & ~static_cast<std::size_t>(TypeInfo::RawType::kObject));
      if (!objTypeInfo)
        throw NullPointerException("objTypeInfo");
      res.className = objTypeInfo->GetName();
    }
    return res;
  }

  const GetNativeFunctionAddr::Result& GetResult()
  {
    if (!result) {
      result = GetNativeFunctionAddr::Run(*f);
    }
    return *result;
  }
};

const std::unique_ptr<FunctionInfo_>& FindFunction(
  FindFunctionCache& cache, const std::string& className,
  const std::string& funcName)
{
  auto& f = cache.funcByFullName[className][funcName];
  if (!f) {
    auto& classInfo = FindType(className);
    if (!classInfo)
      throw std::runtime_error("'" + std::string(className) +
                               "' is not a valid Papyrus class name");

    RE::BSScript::ObjectTypeInfo* p = classInfo.get();
    while (p) {
      auto findRes = FindFunction(p, funcName);
      if (findRes.first) {
        f.reset(new VmFunctionInfo(findRes));
        break;
      }
      p = p->GetParent();
    }
  }
  return f;
}

bool IsDerivedFrom(const RE::BSScript::ObjectTypeInfo& obj,
                   const RE::BSScript::ObjectTypeInfo& requestedParamType)
{
  auto p = &obj;
  while (p) {
    if (!stricmp(p->GetName(), requestedParamType.GetName()))
      return true;
    p = obj.GetParent();
  }
  return false;
}
}

struct VmProvider::Impl
{
  std::unordered_map<std::string, std::optional<bool>> isDerivedFromCache;
  FindFunctionCache findFunctionCache;
};

VmProvider& VmProvider::GetSingleton()
{
  static VmProvider g_instance;
  return g_instance;
}

FunctionInfo_* VmProvider::GetFunctionInfo(const std::string& className,
                                           const std::string& funcName)
{
  return ::FindFunction(pImpl->findFunctionCache, className, funcName).get();
}

bool VmProvider::IsDerivedFrom(const char* derivedClassName,
                               const char* baseClassName)
{
  char buf[1024] = { 0 };
  sprintf_s(buf, "%s.%s", derivedClassName, baseClassName);
  auto& v = pImpl->isDerivedFromCache[buf];
  if (!v.has_value()) {
    auto derived = FindType(derivedClassName);
    auto base = FindType(baseClassName);
    v = derived && base && ::IsDerivedFrom(*derived, *base);
  }
  return *v;
}

VmProvider::VmProvider()
  : pImpl(new Impl)
{
}
