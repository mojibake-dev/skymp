// Compat with pre-SP3 where storage is a Proxy, like Actor/ObjectReference/etc.
// The user code, including skymp5-client and skymp5-gamemode, relies on
// typeof storage[x] === "function" to determine whether the key x is set.
//
// thuum (docs/DECISIONS.md ADR-041, the hot reload): storage must survive a
// plugin reload, as docs/skyrim_platform/features.md promises ("all variables
// are reset, except for storage and its properties") and as skymp5-client is
// written (its world model, id manager, auth data and view live there). This
// script runs again on every (re)load of a plugin, so a fresh `{}` target
// emptied the storage each time and a hot-reloaded client came back with no
// world to send from (the lab, 2026-10-19: the reloaded seat sent no
// movement). The target now lives on the process-wide global, which
// SkyrimPlatform's ClearState never touches, so it is one storage shared by
// every plugin in the folder, as before SP3.
(sp) => {
  const target = globalThis.__skyrimPlatformStorage ||
    (globalThis.__skyrimPlatformStorage = {});
  sp.storage = new Proxy(target, {
    get(target, prop) {
      if (prop in target) {
        return target[prop];
      }

      // Safety: Return undefined for specific symbols or 'then' to prevent
      // the Proxy from breaking async flows or inspection tools.
      // This is a breaking change from pre-SP3 behavior but improves safety.
      if (prop === 'then' || prop === 'toJSON' || typeof prop === 'symbol') {
        return undefined;
      }

      return () => { throw new Error(`Property "${prop}" is not defined in storage`); };
    },

    set(target, prop, value) {
      target[prop] = value;
      return true;
    }
  });
};
