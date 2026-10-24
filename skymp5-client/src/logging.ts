import { printConsole, writeLogs } from "@skyrim-platform/skyrim-platform";
import { ClientListener } from "./services/services/clientListener";

// TODO: redirect this to spdlog
export function logError(service: ClientListener | string, ...rest: unknown[]) {

    const restProcessed = rest.map(item => {
        if (item instanceof Error) {
            return item.stack || item.message;
        }

        return item;
    });

    printConsole(`Error in ${typeof service !== "string" ? service.constructor.name : service}:`, ...restProcessed);
}

// TODO: redirect this to spdlog
export function logTrace(service: ClientListener | string, ...rest: unknown[]) {
    const restProcessed = rest.map(item => {
        if (item instanceof Error) {
            return item.stack || item.message;
        }

        return item;
    });

    printConsole(`Trace in ${typeof service !== "string" ? service.constructor.name : service}:`, ...restProcessed);
}

// thuum docs/LAB.md: a line the lab can count. printConsole (and logTrace,
// which wraps it) reaches the in-game console alone; writeLogs reaches
// Skyrim Platform's log file, the one lab-api's log-count reads (the door
// probe 20261024-013234 counted none of the console lines). Verb lines
// meant for a probe go through this, and still print to the console.
export function logLab(tag: string, ...rest: unknown[]) {
    const line = `[${tag}] ${rest.map((item) => (item instanceof Error ? item.stack || item.message : String(item))).join(" ")}`;
    writeLogs("skymp5-client", line);
    printConsole(line);
}
