import { Game, createText, destroyText, setTextSize } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { ConnectionMessage } from "../events/connectionMessage";
import { HelloMessage, RefuseMessage, WelcomeMessage } from "../messages/sessionMessages";
import { AnyMessage } from "../messages/anyMessage";
import { modHash } from "../../lib/modHash";
import { getScreenResolution } from "../../view/formView";
import { logError, logLab, logTrace } from "../../logging";

// thuum docs/verbs/mod-lock.md (the friend deployment's enforcement): at
// the connection the client says Hello with one hash per plugin, the full
// plugins in load order then the light ones (the engine numbers them
// apart), and the server answers Welcome or Refuse before any login. The
// hash is src/lib/modHash.ts, the one definition shared with the server's
// wire-rules. A refusal is drawn on the screen with its reason; the
// server's log names the plugin. AuthService holds the login until the
// Welcome (or a short wait, for a server without the lock).
export class HelloService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("connectionAccepted", () => this.sendHello());
        this.controller.emitter.on("welcomeMessage", (e) => this.onWelcome(e));
        this.controller.emitter.on("refuseMessage", (e) => this.onRefuse(e));
    }

    private sendHello() {
        this.resetText();
        const full = this.hashes(Game.getModCount, Game.getModName);
        const light = this.hashes(Game.getLightModCount, Game.getLightModName);
        const gameData = this.sp.settings["skymp5-client"]["gameData"] as { profileId?: unknown } | undefined;
        const profileId = gameData && typeof gameData.profileId === "number" ? gameData.profileId : undefined;
        const message: HelloMessage = {
            w: 0,
            client_build: 0,
            mod_hashes: full.concat(light),
            name: profileId === undefined ? "" : `profile-${profileId}`,
        };
        // the session family is not a SkyMP message (no "t"); the networking
        // service sends any JSON object the wire DLL recognizes
        this.controller.emitter.emit("sendMessage", { message: message as unknown as AnyMessage, reliability: "reliable" });
        logLab("verb:mod-lock", `Hello sent: ${full.length} plugins and ${light.length} light`);
    }

    private hashes(getCount: () => number, getAt: (i: number) => string): string[] {
        const out: string[] = [];
        for (let i = 0; i < getCount(); ++i) {
            const filename = getAt(i);
            const info = this.fileInfo(filename);
            out.push(modHash(filename, info.size, info.crc32));
        }
        return out;
    }

    // as LoadOrderVerificationService reads it: a file the platform cannot
    // read hashes as size 0 and crc 0, which the server then refuses by name
    private fileInfo(filename: string): { size: number; crc32: number } {
        try {
            return this.sp.getFileInfo(filename);
        } catch (e) {
            const message = (e as Record<string, unknown>).message;
            if (typeof message === "string" && message.includes("is not a valid argument")) {
                logTrace(this, `no file info for ${filename}`);
                return { size: 0, crc32: 0 };
            }
            throw e;
        }
    }

    private onWelcome(e: ConnectionMessage<WelcomeMessage>) {
        logLab("verb:mod-lock", `Welcome: client ${e.message.client_id}, server time ${e.message.server_time_ms}`);
        this.resetText();
    }

    private onRefuse(e: ConnectionMessage<RefuseMessage>) {
        const reason = e.message.reason;
        const why = reason === 501 ? "E_MOD_LOCK_COUNT: your plugin count differs from the server's"
            : reason === 502 ? "E_MOD_LOCK_MISMATCH: one of your plugins differs from the server's"
            : reason === 503 ? "E_MOD_LOCK_NO_HELLO: the login came before the Hello"
            : `code ${reason}`;
        logError(this, `refused by the mod lock: ${why}`);
        logLab("verb:mod-lock", `Refuse ${reason}`);
        this.printOrder("Client plugins (full):", Game.getModCount, Game.getModName);
        this.printOrder("Client plugins (light):", Game.getLightModCount, Game.getLightModName);
        this.drawText(
            "MOD LOCK: the server refused this load order.\n" + why +
            "\nThe server's log names the plugin; your list is in the console (~).",
        );
    }

    private printOrder(header: string, getCount: () => number, getAt: (i: number) => string) {
        this.sp.printConsole(header);
        for (let i = 0; i < getCount(); ++i) {
            const filename = getAt(i);
            const info = this.fileInfo(filename);
            this.sp.printConsole(`#${i} ${filename} size ${info.size} crc32 ${(info.crc32 >>> 0).toString(16)}`);
        }
    }

    private drawText(text: string) {
        const { width, height } = getScreenResolution();
        this.resetText();
        this.textId = createText(width / 2, height / 2, text, [255, 0, 0, 1]);
        setTextSize(this.textId, 0.5);
    }

    private resetText() {
        if (this.textId !== undefined) {
            destroyText(this.textId);
            this.textId = undefined;
        }
    }

    private textId?: number;
}
