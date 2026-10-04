import { ClientListener, CombinedController, Sp } from "./clientListener";
import { ConnectionMessage } from "../events/connectionMessage";
import { SetGameTimeMessage } from "../messages/setGameTimeMessage";

// The engine's time globals in Skyrim.esm (thuum lab/esm.py, 2026-10-03)
const gameYearId = 0x35;
const gameMonthId = 0x36;
const gameDayId = 0x37;
const gameHourId = 0x38;
const gameDaysPassedId = 0x39;
const timeScaleId = 0x3a;

const msPerHour = 60 * 60 * 1000;
const oneMinute = 1 / 60; // in hours

// Renders the server's game clock (thuum docs/verbs/time.md, ADR-021). The
// server owns the date, the hour and the time scale and says them at login
// and every 60 s; between corrections the engine runs its own clock at the
// server's TimeScale. Before the server has said anything, the engine's
// clock is left alone.
export class TimeService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on("update", () => this.onUpdate());
        controller.emitter.on("setGameTimeMessage", (e) => this.onSetGameTimeMessage(e));
    }

    // The server's time of day now, whole hours, minutes and seconds, for
    // the save the client loads at login; undefined before the server said.
    public getLoginTime(): { hours: number, minutes: number, seconds: number } | undefined {
        const target = this.target();
        if (!target) {
            return undefined;
        }
        const totalSeconds = Math.floor(target.hour * 3600);
        return {
            hours: Math.floor(totalSeconds / 3600),
            minutes: Math.floor(totalSeconds / 60) % 60,
            seconds: totalSeconds % 60,
        };
    }

    private onSetGameTimeMessage(event: ConnectionMessage<SetGameTimeMessage>) {
        this.clock = { message: event.message, receivedAt: Date.now() };
        this.lastTimeUpd = 0; // render it on the next update
    }

    // The latest message advanced by the real time since it came: the hour
    // and the days passed move, the date stays. Past a midnight the date is
    // the next message's business (a minute away at most), so there is no
    // target until it comes.
    private target() {
        if (!this.clock) {
            return undefined;
        }
        const { message, receivedAt } = this.clock;
        const elapsedHours = Math.max(0, Date.now() - receivedAt) / msPerHour * message.timeScale;
        const hour = message.hour + elapsedHours;
        if (hour >= 24) {
            return undefined;
        }
        return {
            year: message.year,
            month: message.month,
            day: message.day,
            hour,
            daysPassed: message.daysPassed + elapsedHours / 24,
            timeScale: message.timeScale,
        };
    }

    private every2seconds() {
        const target = this.target();
        if (!target) {
            return;
        }

        const global = (id: number) => this.sp.GlobalVariable.from(this.sp.Game.getFormEx(id));
        const gameYear = global(gameYearId);
        const gameMonth = global(gameMonthId);
        const gameDay = global(gameDayId);
        const gameHour = global(gameHourId);
        const gameDaysPassed = global(gameDaysPassedId);
        const timeScale = global(timeScaleId);
        if (!gameYear || !gameMonth || !gameDay || !gameHour || !gameDaysPassed || !timeScale) {
            return;
        }

        const dateDiffers = gameYear.getValue() !== target.year
            || gameMonth.getValue() !== target.month
            || gameDay.getValue() !== target.day;
        if (dateDiffers || Math.abs(gameHour.getValue() - target.hour) >= oneMinute) {
            gameYear.setValue(target.year);
            gameMonth.setValue(target.month);
            gameDay.setValue(target.day);
            gameHour.setValue(target.hour);
        }

        // The engine's own GameDaysPassed stops advancing in real time past
        // about 64 days (single precision), so the server's is written
        // whenever the two part by a game minute
        if (Math.abs(gameDaysPassed.getValue() - target.daysPassed) >= oneMinute / 24) {
            gameDaysPassed.setValue(target.daysPassed);
        }

        if (timeScale.getValue() !== Math.fround(target.timeScale)) {
            timeScale.setValue(target.timeScale);
        }
    }

    private onUpdate() {
        if (Date.now() - this.lastTimeUpd <= 2000) {
          return;
        }
        this.lastTimeUpd = Date.now();
        this.every2seconds();
    }

    private clock?: { message: SetGameTimeMessage, receivedAt: number };
    private lastTimeUpd = 0;
}
