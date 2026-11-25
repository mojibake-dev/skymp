// What an E press found under the crosshair when it found no actor to talk
// to (talkService.ts). JS state alone.

// the reference the crosshair held, as the engine has it
export interface CrosshairHeld {
    formId: number;
    // the reference's own form type (CommonLibSSE-NG include/RE/F/FormTypes.h)
    formType: number;
    // its base object's form id and form type, 0 for none
    baseId: number;
    baseType: number;
    // from the player, in units
    distance: number;
}

// the trace line for an E that found no actor: nothing under the crosshair,
// or what it held
export function missLine(held: CrosshairHeld | null): string {
    void held;
    return "E: nothing or no actor under the crosshair";
}
