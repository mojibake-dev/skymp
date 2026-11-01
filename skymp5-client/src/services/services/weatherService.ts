import { Weather } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { SetWeatherMessage } from '../messages/setWeatherMessage';
import { logError, logTrace } from '../../logging';

// thuum docs/verbs/weather.md (M4): the sky is the server's. The server picks
// one weather per worldspace on its game clock (wire-rules weather) and tells
// this game the weather of the place it stands in: at login, when it enters a
// place, and at each change. This service forces that weather on the engine
// (Weather.forceActive for a cut, as at login or on entering a place;
// Weather.setActive with the blend for a change the player stood through) and
// keeps the override, so the engine's own rolls are off while the server
// speaks. A weather of 0 releases the override (an interior, where the cell's
// own sky stands anyway; a worldspace without a pool). A word for a place this
// game is not in yet (the server saw the cell change first) is kept and
// applied when the player gets there. Every two seconds the forced weather is
// checked against the engine's current one and forced again if the engine
// moved off it (a load, a mod); a blend in progress is left to finish
// (HYPOTHESIS: Weather.getCurrentWeatherTransition reads below 1 through a
// blend; the probe x-weather reads it). Lines for the lab's log counts:
// "weather applied", "weather released", "weather word for ... kept".
interface Word {
  message: SetWeatherMessage;
  applied: boolean;
  reapplied: number;
  // when the word arrived (Date.now()): the tick's late apply waits a tick
  // for the word's own deferred apply
  heardAt: number;
}

const hex = (n: number) => n.toString(16);

export class WeatherService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    this.controller.emitter.on('setWeatherMessage', (e) =>
      this.onSetWeather(e),
    );
    this.controller.on('update', () => this.onUpdate());
  }

  private onSetWeather(e: ConnectionMessage<SetWeatherMessage>) {
    this.word = {
      message: e.message,
      applied: false,
      reapplied: 0,
      heardAt: Date.now(),
    };
    // the apply runs on the next update: a message handler has no Papyrus VM
    // context and Skyrim Platform refuses natives there ("can't be called in
    // this context", the locks prompt's lesson of x-lockpick 20261031-144606)
    this.controller.once('update', () => this.apply('word'));
  }

  private onUpdate() {
    const now = Date.now();
    if (now - this.lastTick < WeatherService.tickMs) {
      return;
    }
    this.lastTick = now;
    const word = this.word;
    if (!word) {
      return;
    }
    if (!word.applied) {
      // a word kept for a place this game is not in yet (an interior's exit
      // brings it back) applies here; a word just heard waits for its own
      // deferred apply, else the tick's forced cut lands first and a
      // transition never blends (x-weather 20261031-171309: every login
      // word applied twice, late then word, 18 ms apart)
      if (now - word.heardAt >= WeatherService.tickMs) {
        this.apply('late');
      }
      return;
    }
    if (word.message.weather === 0 || this.place() !== word.message.place) {
      return;
    }
    const current = Weather.getCurrentWeather();
    if (current && current.getFormID() === word.message.weather) {
      return;
    }
    if (Weather.getCurrentWeatherTransition() < 1) {
      return;
    }
    this.apply('re-applied');
  }

  // the worldspace the player stands in, else its interior cell, else 0
  private place(): number {
    const player = this.sp.Game.getPlayer();
    if (!player) {
      return 0;
    }
    const world = player.getWorldSpace();
    if (world) {
      return world.getFormID();
    }
    const cell = player.getParentCell();
    return cell ? cell.getFormID() : 0;
  }

  private apply(how: 'word' | 'late' | 're-applied') {
    const word = this.word;
    if (!word) {
      return;
    }
    const m = word.message;
    const here = this.place();
    if (here !== m.place) {
      if (how === 'word') {
        logTrace(
          this,
          `weather word for ${hex(m.place)} kept: this game is in ${hex(here)}`,
        );
      }
      return;
    }
    if (m.weather === 0) {
      Weather.releaseOverride();
      word.applied = true;
      logTrace(this, `weather released in ${hex(m.place)} (${how})`);
      return;
    }
    const weather = Weather.from(this.sp.Game.getFormEx(m.weather));
    if (!weather) {
      word.applied = true;
      logError(
        this,
        `weather ${hex(m.weather)} is not in this game's files; the engine's own sky stands`,
      );
      return;
    }
    if (m.transition && how === 'word') {
      weather.setActive(true, false);
    } else {
      weather.forceActive(true);
    }
    word.applied = true;
    if (how === 're-applied') {
      word.reapplied += 1;
    }
    logTrace(
      this,
      `weather applied ${hex(m.weather)} to ${hex(m.place)} (${m.transition ? 'transition' : 'cut'}, ${how}${how === 're-applied' ? ` ${word.reapplied}` : ''})`,
    );
  }

  private word: Word | undefined;
  private lastTick = 0;
  private static readonly tickMs = 2000;
}
