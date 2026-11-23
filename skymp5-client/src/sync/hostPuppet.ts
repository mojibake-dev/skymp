// Whether this seat runs an actor's AI (PuppetService.reconcile): the server's
// hosting word, except while another seat's stream owns the actor's knocked
// body (a grip's or a throw's live-body row). Then this seat's copy is held as
// a puppet, as every other seat's copy is: its AI off, so it neither attacks
// nor reports moves the server would refuse from a seat that does not own the
// body, and the owner's stream alone moves it.
export const runsActorHere = (f: { hostedHere: boolean; bodyOwnedElsewhere: boolean }): boolean =>
  f.hostedHere && !f.bodyOwnedElsewhere;

// Whether the promote that ends a puppet period places the actor at the
// record's last movement sample. Not after a hold for another seat's stream of
// its body: the follow's end already set the copy at the owner's rest (the
// stream's last root), and the record's sample is where it stood before the
// grip or the throw. Not a seated actor either: its furniture places it.
export const placesAtPromote = (f: { heldForBody: boolean; hasSample: boolean; seated: boolean }): boolean =>
  f.hasSample && !f.seated && !f.heldForBody;
