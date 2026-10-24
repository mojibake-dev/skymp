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

// thuum docs/LAB.md: a line the lab can count. Since lab main 70731404 the
// driver tees every console line into <client>-console.log and log-count
// reads it beside skyrim-platform.log, so printConsole is what a probe
// counts; writeLogs writes Data/Platform/Logs/<plugin>-logs.txt, a file
// nothing reads, kept here as a second copy. Verb lines meant for a probe
// go through this.
export function logLab(tag: string, ...rest: unknown[]) {
    const line = `[${tag}] ${rest.map((item) => (item instanceof Error ? item.stack || item.message : String(item))).join(" ")}`;
    writeLogs("skymp5-client", line);
    printConsole(line);
}
