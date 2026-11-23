// The name a stack keeps as its own, the one the server records and addItemEx
// writes back. The engine's display name of a tempered item is its own name,
// or its base's, with one temper label after it (" (Fine)"; ExtraTextDisplayData's
// displayName, whose customNameLength counts the name without the temper string:
// CommonLibSSE-NG include/RE/E/ExtraTextDisplayData.h:39, :44). The label is the
// engine's to add from the stack's health, so a tempered stack's last bracketed
// label is cut, and a name that is then its base's own is none
export const ownName = (name: string | undefined, health: number | undefined, baseName: string): string | undefined => {
  if (!name) {
    return undefined;
  }
  let own = name;
  if (health !== undefined && health > 1) {
    const labelled = /^(.*) \([^()]*\)$/.exec(name);
    if (labelled) {
      own = labelled[1];
    }
  }
  return own === baseName || own === "" ? undefined : own;
};
