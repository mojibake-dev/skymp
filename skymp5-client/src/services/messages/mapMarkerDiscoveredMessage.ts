import { MsgType } from "../../messages";

// The player's engine discovered a location of this marker type (thuum
// docs/verbs/map-markers.md). The server finds which marker near the player
// and records it; every login shows the player's markers again.
export interface MapMarkerDiscoveredMessage {
    t: MsgType.MapMarkerDiscovered;
    markerType: number; // the engine's MARKER_TYPE
    canTravel: boolean; // fast travel allowed to it
}
