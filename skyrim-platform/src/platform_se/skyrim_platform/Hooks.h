#pragma once
#include <utility>
#include <vector>

namespace Hooks {

template <class T>
void write_thunk_call(std::uintptr_t a_src)
{
  auto& trampoline = SKSE::GetTrampoline();
  T::func = trampoline.write_call<5>(a_src, T::thunk);
}

void Install();
// thuum docs/verbs/cast-action.md: the sink on SKSE's action events that
// records a figure's graph answering a cast request; at kDataLoaded
void InstallCastAnswerSink();

std::vector<std::tuple<std::string, std::string, RE::BSScript::IFunction*,
                       uintptr_t, uintptr_t, uintptr_t>>
GetBoundNatives();

// thuum train 52, Fix 1: SkyrimScript::AddItemFunctor's operator() slot,
// written at Install (Hooks.cpp): what the slot held before the write, as its
// allocation base and the offset from it ({0, 0} before Install), for the
// seam's install line; and whether the slot holds the platform's thunk now,
// the seam's read-back at its first parked call (H-LR2)
std::pair<std::uintptr_t, std::uintptr_t> AddItemFunctorSlotWas();
bool AddItemFunctorSlotHolds();

}
