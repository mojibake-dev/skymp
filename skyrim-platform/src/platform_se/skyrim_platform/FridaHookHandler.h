#pragma once

namespace Frida {
typedef struct _InvocationListener InvocationListener;

struct _InvocationListener
{
  GObject parent;
  guint num_calls;
};

enum HookID
{
  HOOK_SEND_ANIMATION_EVENT,
  DRAW_SHEATHE_WEAPON_ACTOR,
  DRAW_SHEATHE_WEAPON_PC,
  QUEUE_NINODE_UPDATE,
  APPLY_MASKS_TO_RENDER_TARGET,
  RENDER_CURSOR_MENU,
  SEND_EVENT,
  SEND_EVENT_ALL,
  CONSOLE_VPRINT,
  NATIVE_FUNCTION_CTOR,
  // thuum docs/verbs/cast-action.md
  HOOK_PERFORM_ACTION,
  // thuum docs/verbs/marksman.md: every projectile launch
  HOOK_PROJECTILE_LAUNCH,
  // and an arrow's final angles on its first update
  HOOK_PROJECTILE_INIT_VELOCITY,
  // thuum docs/research/apocalypse/target-location.md: where a
  // target-location cast places its explosion
  HOOK_PLACE_AREA_EFFECT,
  // thuum docs/verbs/npc-fate.md: the engine's reanimation of a body
  // (docs/research/m3/re-resurrect-hook.md, Address Library 38820)
  HOOK_REANIMATE,
  // thuum docs/verbs/prop-physics.md: the player's character controller's
  // contact with a prop's body (docs/research/m3/re-havok-contact.md,
  // Address Library 79131)
  HOOK_PROP_CONTACT
};

struct Hook
{
  Hook(std::function<void(GumInvocationContext*)> enter,
       std::function<void(GumInvocationContext*)> leave)
    : onEnter(enter)
    , onLeave(leave)
  {
  }
  std::function<void(GumInvocationContext*)> onEnter;
  std::function<void(GumInvocationContext*)> onLeave;
};

class HookHandler
{
public:
  static HookHandler* GetSingleton()
  {
    static HookHandler singleton;
    return &singleton;
  }

  bool Attach(HookID id, uintptr_t address);
  void Install(HookID id, uintptr_t address, std::shared_ptr<Hook> hook);
  // Replace the function at address with replacement (Gum's
  // gum_interceptor_replace, frida-gum 16.0.19 gum/guminterceptor.h:49-51),
  // the original left callable through *original; false when Gum refused
  // (thuum docs/verbs/figure-values.md, the first replace-style hook)
  bool Replace(uintptr_t address, void* replacement, void** original);
  auto GetHooks() { return &hooks; }
  std::function<void(GumInvocationContext*)> GetHookEnterFunction(HookID id);
  std::function<void(GumInvocationContext*)> GetHookLeaveFunction(HookID id);

private:
  HookHandler();

  // with current impl this detaches all hooks
  void Detach()
  {
    gum_interceptor_begin_transaction(_interceptor);
    gum_interceptor_detach(_interceptor, _listener);
    gum_interceptor_end_transaction(_interceptor);
  }

  robin_hood::unordered_map<HookID, std::shared_ptr<Hook>> hooks;

  GumInterceptor* _interceptor;
  GumInvocationListener* _listener;
};
}
