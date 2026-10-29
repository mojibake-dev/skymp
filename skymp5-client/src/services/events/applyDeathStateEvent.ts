import { Actor } from "skyrimPlatform";

export interface ApplyDeathStateEvent {
    actor: Actor;
    isDead: boolean;
    // the bind kill's word (formView's dead check, playtest 37's finding: a
    // seat entering a cell heard a death sound for a body the record already
    // held dead): the actor dies by KillSilent, no death events, no push
    silent?: boolean;
}
