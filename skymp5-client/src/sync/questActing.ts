// Each quest's acting player as this seat last learned it: the acting stamp of
// its latest start or stage. This seat's own report names the player whose act
// it was (the platform's acting scope), none meaning this seat's own; a relay
// carries the server's stamp, and an unstamped relay leaves the quest's seat
// unknown. A quest's scenes run on the game of its acting player, which claims
// their actors: a scene's phase conditions and packages read "the player",
// and only that player's own game has that player as its player. JS state
// alone.

export type QuestSeat = "here" | "other" | "unknown";

// how this seat saw a scene: a start with no acting player, a start inside
// another player's acting scope, an action on an actor this seat simulates,
// an action on a puppet here
export type SceneSeen = "start" | "start-acting" | "action-mine" | "action-puppet";

export const QUEST_ACTING_MAX = 512;

export class QuestActing {
  private readonly seats = new Map<number, { here: boolean; acting: number | undefined }>();

  constructor(private readonly max = QUEST_ACTING_MAX) {}

  // this seat's own report of a start or a stage: the acting player the
  // platform named, none for this seat's own player; `me` when that player
  // is this seat's own
  noteReport(quest: number, acting: number | undefined, me: boolean): void {
    this.set(quest, acting === undefined || me, acting);
  }

  // a relayed start or stage: the server's stamp, `me` when it names this
  // seat's own player; an unstamped relay leaves the seat unknown
  noteRelay(quest: number, acting: number | undefined, me: boolean): void {
    if (acting === undefined) {
      this.seats.delete(quest);
      return;
    }
    this.set(quest, me, acting);
  }

  seatOf(quest: number): QuestSeat {
    const seat = this.seats.get(quest);
    return seat === undefined ? "unknown" : seat.here ? "here" : "other";
  }

  actingOf(quest: number): number | undefined {
    return this.seats.get(quest)?.acting;
  }

  clear(): void {
    this.seats.clear();
  }

  // the latest note last in order, so the one noted longest ago goes past
  // the cap
  private set(quest: number, here: boolean, acting: number | undefined): void {
    this.seats.delete(quest);
    if (this.seats.size >= this.max) {
      const oldest = this.seats.keys().next();
      if (!oldest.done) {
        this.seats.delete(oldest.value);
      }
    }
    this.seats.set(quest, { here, acting });
  }
}

// the one table, QuestStageService's notes and SceneClaimService's reads
export const questActing = new QuestActing();

// whether a scene is this seat's own: by its quest's seat when known, else
// by how this seat saw it (a start with no acting player or an action on an
// actor this seat simulates is its own, a start inside another player's
// acting scope is that seat's, an action on a puppet marks nothing)
export const sceneOwnHere = (seat: QuestSeat, seen: SceneSeen): boolean | undefined => {
  if (seat !== "unknown") {
    return seat === "here";
  }
  if (seen === "start" || seen === "action-mine") {
    return true;
  }
  return seen === "start-acting" ? false : undefined;
};

// the scope a live relay's apply arms: another player's acting scope for a
// stamp naming another player; none for this seat's own player (its
// fragment's scenes start here and are claimed here); a relayed scope with
// no swap for an unstamped relay
export const relayScope = (acting: number | undefined, me: boolean): "acting" | "none" | "relayed" =>
  acting === undefined ? "relayed" : me ? "none" : "acting";
