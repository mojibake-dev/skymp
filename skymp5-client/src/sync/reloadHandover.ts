// A hot reload's hand-over between the outgoing client and the incoming one.
// The outgoing client's world view is kept across the reload and torn down by
// the incoming one at its first update; its form views run the outgoing
// bundle's code, so whatever they call goes to the outgoing client's own
// services. JS state alone.

// Why the form views are let go: the model no longer holds the form (the
// server took it back from this game), the player changed worldspace, or a
// reload's teardown of the kept view.
export type Teardown = "model" | "world" | "reload";

// Whether a teardown is a loss: a loss demotes a game-file actor the view
// held live here (formView.ts destroy). A reload's teardown hands its forms
// to the incoming view and loses none.
export function teardownLoses(why: Teardown): boolean {
    return why !== "reload";
}

// The actors the incoming client's restored hosted set holds that the
// platform's hosted set lost: an outgoing client's teardown of its views
// demotes through its own services, which clear the platform's set while the
// restored one keeps them. Set again, and taken live again, once those
// demotes have run.
export function lostAtReload(restored: Iterable<number>, platformHosts: (id: number) => boolean): number[] {
    return Array.from(restored).filter((id) => !platformHosts(id));
}
