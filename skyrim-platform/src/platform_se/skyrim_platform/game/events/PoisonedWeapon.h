#pragma once

namespace RE {

// thuum docs/verbs/poison.md: the source of the engine's PoisonedWeapon
// story event, which CommonLibSSE-NG declares (include/RE/P/PoisonedWeapon.h,
// an empty event) without a source. The engine sends it when its player
// answers Yes in the poison box, after poisoning the right hand's weapon
// (the re-analyst's read, 2026-10-12). The source's function is Address
// Library 41069 on 1.6.1170 (lab/addr.py); its SE id is unread, so on any
// other runtime there is no source and the sink stays unregistered.
struct PoisonedWeaponSource
{
public:
  using Event = PoisonedWeapon::Event;

  static RE::BSTEventSource<Event>* GetEventSource()
  {
    if (!REL::Module::IsAE()) {
      return nullptr;
    }
    using func_t = decltype(&PoisonedWeaponSource::GetEventSource);
    REL::Relocation<func_t> func{ Offsets::EventSource::PoisonedWeapon };
    return func();
  }
};

}
