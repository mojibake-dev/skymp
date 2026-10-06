import { MenuCloseEvent } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { FavoriteEntry, FavoritesMessage } from "../messages/favoritesMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { isBaseReset } from "../../sync/inventory";
import { isBadMenuShown } from "../../sync/equipment";

// thuum docs/verbs/favorites.md: the items and magic a player marked as
// favorites, and their hotkeys, survive a login. The service keeps the list
// the player wants: the server's record after a login, then the engine's own
// each time the inventory, magic or favorites menu closes, which is reported
// when it changed. SkyMP rebuilds the player's inventory now and then (the
// first sync after a login empties and refills it, sync/inventory.ts
// resetBase; applyEquipment empties it at a login), and an item's mark goes
// with its entry (a-favorites, runs 20261006-055012 and -070238: the dagger's
// mark lost after a relaunch, Flames kept). So every two seconds, outside
// those menus, the marks the engine lost are put back: items the player
// holds and magic it knows (TESModPlatform.SetFavorite refuses the rest).
export class FavoritesService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("favoritesMessage", (e) => this.onFavoritesMessage(e));
        this.controller.on("menuClose", (e) => this.onMenuClose(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onMenuClose(e: MenuCloseEvent) {
        if (FavoritesService.menus.indexOf(e.name) === -1) {
            return;
        }
        const entries = this.read();
        if (!entries) {
            return;
        }
        // what the player just left in the menu is what it wants
        this.desired = entries;
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
        this.desired = e.message.entries.slice();
        // the server's record counts as sent: it is not echoed back
        this.lastKey = FavoritesService.keyOf(this.desired);
    }

    private onUpdate() {
        if (this.desired.length === 0 || Date.now() - this.lastCheck < 2000) {
            return;
        }
        this.lastCheck = Date.now();
        const player = this.sp.Game.getPlayer();
        if (!player || !isBaseReset(player) || isBadMenuShown()) {
            return;
        }
        const current = this.read();
        if (!current) {
            return;
        }
        const have: Record<string, boolean> = {};
        current.forEach((x) => { have[x.form + ":" + x.hotkey] = true; });
        this.desired.forEach((entry) => {
            if (!have[entry.form + ":" + entry.hotkey]) {
                this.mark(entry);
            }
        });
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

    private desired: FavoriteEntry[] = [];
    private lastCheck = 0;
    private lastKey = "";
}
