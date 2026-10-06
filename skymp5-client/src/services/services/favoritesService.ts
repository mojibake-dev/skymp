import { MenuCloseEvent } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { FavoriteEntry, FavoritesMessage } from "../messages/favoritesMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// thuum docs/verbs/favorites.md: the items and magic a player marked as
// favorites, and their hotkeys, survive a login. After the inventory, magic
// or favorites menu closes, the engine's favorites are read once
// (TESModPlatform.GetFavorites) and reported when they changed. After a
// login the server sends its record; an item can be missing from the
// inventory at first (the client re-applies the server's inventory on a 5 s
// timer, remoteServer.ts), so each entry the engine refuses
// (TESModPlatform.SetFavorite answers false) is tried again every second for
// a minute, and no report goes out until then, so a half-marked list never
// replaces the record.
export class FavoritesService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("favoritesMessage", (e) => this.onFavoritesMessage(e));
        this.controller.on("menuClose", (e) => this.onMenuClose(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onMenuClose(e: MenuCloseEvent) {
        if (FavoritesService.menus.indexOf(e.name) === -1 || this.pending.length > 0) {
            return;
        }
        const entries = this.read();
        if (!entries) {
            return;
        }
        const key = FavoritesService.keyOf(entries);
        if (key === this.lastKey) {
            return;
        }
        this.lastKey = key;
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.Favorites, entries },
            reliability: "reliable",
        });
    }

    private onFavoritesMessage(e: ConnectionMessage<FavoritesMessage>) {
        this.pending = e.message.entries.slice();
        this.pendingUntil = Date.now() + 60000;
        // the server's record counts as sent: it is not echoed back
        this.lastKey = FavoritesService.keyOf(e.message.entries);
    }

    private onUpdate() {
        if (this.pending.length === 0 || Date.now() - this.lastTry < 1000) {
            return;
        }
        this.lastTry = Date.now();
        const left: FavoriteEntry[] = [];
        for (const entry of this.pending) {
            if (!this.mark(entry)) {
                left.push(entry);
            }
        }
        if (left.length > 0 && Date.now() < this.pendingUntil) {
            this.pending = left;
            return;
        }
        if (left.length > 0) {
            logTrace(this, "Favorites the engine never took:", left.map((x) => x.form.toString(16)).join(","));
        }
        this.pending = [];
    }

    private mark(entry: FavoriteEntry): boolean {
        const form = this.sp.Game.getFormEx(entry.form);
        if (!form) {
            return false;
        }
        try {
            return this.sp.callNative("TESModPlatform", "SetFavorite", undefined, form, entry.hotkey) === true;
        } catch (err) {
            logError(this, "TESModPlatform.SetFavorite failed", err);
            return false;
        }
    }

    // TESModPlatform.GetFavorites answers form id, hotkey, form id, hotkey...
    private read(): FavoriteEntry[] | undefined {
        try {
            const raw = this.sp.callNative("TESModPlatform", "GetFavorites", undefined) as number[];
            const out: FavoriteEntry[] = [];
            for (let i = 0; i + 1 < raw.length; i += 2) {
                out.push({ form: raw[i] >>> 0, hotkey: raw[i + 1] });
            }
            return out;
        } catch (err) {
            logError(this, "TESModPlatform.GetFavorites failed", err);
            return undefined;
        }
    }

    private static keyOf(entries: FavoriteEntry[]): string {
        return entries.map((e) => e.form + ":" + e.hotkey).sort().join(",");
    }

    // the menus where a player marks favorites or binds their keys
    private static readonly menus = ["InventoryMenu", "MagicMenu", "FavoritesMenu"];

    private pending: FavoriteEntry[] = [];
    private pendingUntil = 0;
    private lastTry = 0;
    private lastKey = "";
}
