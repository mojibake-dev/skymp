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
    if (!held) {
        return "E: nothing under the crosshair";
    }
    return `E: no actor under the crosshair: ${held.formId.toString(16)} (form type ${held.formType}), ` +
        `base ${held.baseId.toString(16)} (form type ${held.baseType}), ${held.distance.toFixed(0)} units from the player`;
}
