import { ClientListener, CombinedController, Sp } from "./clientListener";
import { LocationDiscoveryEvent } from "skyrimPlatform";
import { MsgType } from "../../messages";

// thuum docs/verbs/map-markers.md: the player's engine discovered a location.
// The client reports only its marker type and fast travel flag; the server
// finds which marker from the master files and the player's position,
// records it, and shows the player's markers again after every login
// (ObjectReference.AddToMap snippets).
export class MapMarkersService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("locationDiscovery", (e) => this.onLocationDiscovery(e));
    }

    private onLocationDiscovery(e: LocationDiscoveryEvent) {
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.MapMarkerDiscovered, markerType: e.markerType, canTravel: e.canTravelTo },
            reliability: "reliable",
        });
    }
}
