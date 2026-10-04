import { ClientListener, CombinedController, Sp } from "./clientListener";
import { ConnectionMessage } from "../events/connectionMessage";
import { SetGameTimeMessage } from "../messages/setGameTimeMessage";
import { MenuOpenEvent } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError } from "../../logging";

// The engine's time globals in Skyrim.esm (thuum lab/esm.py, 2026-10-03)
const gameYearId = 0x35;
const gameMonthId = 0x36;
const gameDayId = 0x37;
const gameHourId = 0x38;
const gameDaysPassedId = 0x39;
const timeScaleId = 0x3a;

// RE::SleepWaitMenu::MENU_NAME (CommonLibSSE-NG include/RE/S/SleepWaitMenu.h)
const sleepWaitMenu = "Sleep/Wait Menu";

const msPerHour = 60 * 60 * 1000;
const oneMinute = 1 / 60; // in hours
// Day counts are whole days apart when they are wrong (a save's, a fast
// travel's); this is far above single precision's spacing near 16,000 days
const dayCountTolerance = 0.01;

// Renders the server's game clock (thuum docs/verbs/time.md, ADR-021). The
// server owns the date, the hour, the day count and the time scale and says
// them at login and every 60 s. Between corrections the engine runs its own
// clock at the server's TimeScale, rolling the date at midnight itself.
//
// The engine rebuilds GameDaysPassed every frame from a day count of its own
// plus GameHour / 24, so a SetValue on that global lasts one frame; the day
// count is set through TESModPlatform.SetGameDaysPassed instead. The server's
// day count carries the hour the same way (whole days plus the hour over 24),
// so it runs on unbroken across midnight and is what the two are compared by.
// Before the server has said anything, the engine's clock is left alone.
//
// A rest (thuum docs/verbs/rest.md) is the player's own: the Sleep/Wait menu
// runs the engine's clock ahead, and the correction holds off while the menu
// is open. After it closes, the whole hours the engine is ahead are the rest,
// reported to the server, which grants its recovery; then the clock is
// corrected as always. Only a Sleep/Wait menu counts, so a fast travel's
// hours are corrected without a rest.
export class TimeService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on("update", () => this.onUpdate());
        controller.on("menuOpen", (e) => this.onMenuOpen(e));
        controller.emitter.on("setGameTimeMessage", (e) => this.onSetGameTimeMessage(e));
    }

    private onMenuOpen(event: MenuOpenEvent) {
        if (event.name === sleepWaitMenu) {
            this.restMenuSeen = true;
        }
    }

    // The server's time of day now, whole hours, minutes and seconds, for
    // the save the client loads at login; undefined before the server said.
    public getLoginTime(): { hours: number, minutes: number, seconds: number } | undefined {
        if (!this.clock) {
            return undefined;
        }
        const hour = Math.min(this.clock.message.hour + this.elapsedHours(this.clock), 24 - 1 / 3600);
        const totalSeconds = Math.floor(hour * 3600);
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

    // Game hours since the message came: the real time since, at its time
    // scale
    private elapsedHours(clock: { message: SetGameTimeMessage, receivedAt: number }) {
        return Math.max(0, Date.now() - clock.receivedAt) / msPerHour * clock.message.timeScale;
    }

    private every2seconds() {
        if (!this.clock) {
            return;
        }
        if (this.sp.Ui.isMenuOpen(sleepWaitMenu)) {
            return; // a rest in progress; it is read when the menu closes
        }
        const message = this.clock.message;

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

        if (timeScale.getValue() !== Math.fround(message.timeScale)) {
            timeScale.setValue(message.timeScale);
        }

        // Hours since midnight of the message's day: past 24 a midnight has
        // gone by since it came. Past 48 the date is the next message's
        // business (it is a minute away), so nothing is touched.
        const elapsedHours = this.elapsedHours(this.clock);
        const targetHours = message.hour + elapsedHours;
        const targetDays = message.daysPassed + elapsedHours / 24;
        if (targetHours >= 48) {
            return;
        }

        if (this.restMenuSeen) {
            this.restMenuSeen = false;
            const hours = Math.round((gameDaysPassed.getValue() - targetDays) * 24);
            if (hours >= 1) {
                // a sleep when the player is still in the bed it slept in
                const sleep = !!this.sp.Game.getPlayer()?.getFurnitureReference();
                this.controller.emitter.emit("sendMessage", {
                    message: { t: MsgType.RestIntent, hours: Math.min(hours, 24), sleep },
                    reliability: "reliable",
                });
            }
        }

        const onMessageDate = gameYear.getValue() === message.year
            && gameMonth.getValue() === message.month
            && gameDay.getValue() === message.day;
        if (Math.abs(gameDaysPassed.getValue() - targetDays) < dayCountTolerance) {
            // The day count is in step, so the engine is on the message's
            // date or has rolled past its midnight once
            const engineHours = onMessageDate ? gameHour.getValue() : gameHour.getValue() + 24;
            if (Math.abs(engineHours - targetHours) < oneMinute) {
                return;
            }
        }

        // The message's date and the hour since its midnight: an hour past 24
        // has the engine's next clock step roll the date itself (the day, the
        // month and year at their ends, its day count, the Days Passed stat).
        // The day count goes last, as it is set against the hour.
        gameYear.setValue(message.year);
        gameMonth.setValue(message.month);
        gameDay.setValue(message.day);
        gameHour.setValue(targetHours);
        try {
            this.sp.callNative("TESModPlatform", "SetGameDaysPassed", undefined, targetDays);
        } catch (e) {
            logError(this, "TESModPlatform.SetGameDaysPassed failed", e);
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
    private restMenuSeen = false;
}
