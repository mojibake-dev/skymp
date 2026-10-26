#include "Hosted.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {
struct
{
  std::unordered_set<uint32_t> ids;
  std::unordered_map<uint32_t, int> performing;
  std::recursive_mutex m;
} g_hosted;
}

bool Hosted::Is(uint32_t formId)
{
  std::lock_guard l(g_hosted.m);
  return g_hosted.ids.count(formId) != 0;
}

void Hosted::Set(uint32_t formId, bool hosted)
{
  std::lock_guard l(g_hosted.m);
  if (hosted) {
    g_hosted.ids.insert(formId);
  } else {
    g_hosted.ids.erase(formId);
  }
}

size_t Hosted::Count()
{
  std::lock_guard l(g_hosted.m);
  return g_hosted.ids.size();
}

void Hosted::BeginPerforming(uint32_t formId)
{
  std::lock_guard l(g_hosted.m);
  ++g_hosted.performing[formId];
}

void Hosted::EndPerforming(uint32_t formId)
{
  std::lock_guard l(g_hosted.m);
  const auto it = g_hosted.performing.find(formId);
  if (it == g_hosted.performing.end()) {
    return;
  }
  if (--it->second <= 0) {
    g_hosted.performing.erase(it);
  }
}

bool Hosted::IsPerforming(uint32_t formId)
{
  std::lock_guard l(g_hosted.m);
  const auto it = g_hosted.performing.find(formId);
  return it != g_hosted.performing.end() && it->second > 0;
}
