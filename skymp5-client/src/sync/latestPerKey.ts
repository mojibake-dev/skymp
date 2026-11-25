// The newest value of each key until it is taken. A container's record comes
// whole in each of the server's sends, and its apply lays the difference
// between that record and the engine's container through
// TESModPlatform.addItemEx, which queues each change for the game thread: a
// second apply in the same tick reads the container before the first one's
// changes land and lays the same difference again. So a burst of one
// container's records within a tick is kept as its newest and applied once.
export class LatestPerKey<V> {
  private readonly pending = new Map<number, V>();

  // keeps `value` as the key's newest; true when nothing was pending for the
  // key, so its one apply is still to be scheduled
  put(key: number, value: V): boolean {
    const fresh = !this.pending.has(key);
    this.pending.set(key, value);
    return fresh;
  }

  // the key's newest value, now no longer pending; undefined for none
  take(key: number): V | undefined {
    const value = this.pending.get(key);
    this.pending.delete(key);
    return value;
  }
}
