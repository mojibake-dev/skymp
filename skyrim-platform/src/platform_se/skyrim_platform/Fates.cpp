#include "Fates.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {
struct
{
  std::unordered_map<uint32_t, Fates::Fate> fates;
  std::recursive_mutex m;
} g_fates;

// tested by both detours on every roll and every outfit init before they
// take the table's lock, so the off state costs one load
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
