import { MsgType } from "../../messages";

// A player's favorites, the whole list (thuum docs/verbs/favorites.md). The
// client sends it after the inventory, magic or favorites menu closed and the
// list changed; the server sends its record after a login.
export interface FavoritesMessage {
    t: MsgType.Favorites;
    entries: FavoriteEntry[];
}

export interface FavoriteEntry {
    form: number; // the form id, as the sender knows it
    hotkey: number; // -1 for none, 0 to 7 for the keys 1 to 8
}
