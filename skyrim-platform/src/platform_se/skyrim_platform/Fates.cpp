#include "Fates.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace {
struct
{
  std::unordered_map<uint32_t, Fates::Fate> fates;
  // the fates armed for copies not yet spawned, by base (ADR-050 decision 3)
  std::unordered_map<uint32_t, std::deque<Fates::Fate>> armed;
  std::recursive_mutex m;
} g_fates;

// tested by both detours on every leveled roll and every outfit resolution
// before they take the table's lock, so the off state costs one load
std::atomic<bool> g_enabled{ false };
}

bool Fates::Enabled()
{
  return g_enabled.load(std::memory_order_relaxed);
}

void Fates::SetEnabled(bool on)
{
  g_enabled.store(on, std::memory_order_relaxed);
}

void Fates::Set(uint32_t refrId, const Fate& fate)
{
  std::lock_guard l(g_fates.m);
  g_fates.fates[refrId] = fate;
}

bool Fates::Clear(uint32_t refrId)
{
  std::lock_guard l(g_fates.m);
  return g_fates.fates.erase(refrId) != 0;
}

bool Fates::Get(uint32_t refrId, Fate& out)
{
  std::lock_guard l(g_fates.m);
  const auto it = g_fates.fates.find(refrId);
  if (it == g_fates.fates.end()) {
    return false;
  }
  out = it->second;
  return true;
}

size_t Fates::Count()
{
  std::lock_guard l(g_fates.m);
  return g_fates.fates.size();
}

void Fates::Arm(uint32_t baseId, const Fate& fate)
{
  std::lock_guard l(g_fates.m);
  g_fates.armed[baseId].push_back(fate);
}

bool Fates::BindArmed(RE::TESObjectREFR* ref)
{
  // TESForm::formID (include/RE/T/TESForm.h:354); the copy's base object
  // (include/RE/T/TESObjectREFR.h:383)
  if (!ref || ref->formID < 0xff000000) {
    return false;
  }
  auto* base = ref->GetBaseObject();
  if (!base) {
    return false;
  }
  std::lock_guard l(g_fates.m);
  if (g_fates.fates.count(ref->formID) != 0) {
    return false;
  }
  const auto it = g_fates.armed.find(base->formID);
  if (it == g_fates.armed.end() || it->second.empty()) {
    return false;
  }
  g_fates.fates[ref->formID] = it->second.front();
  it->second.pop_front();
  if (it->second.empty()) {
    g_fates.armed.erase(it);
  }
  return true;
}
