import { Game, Utility, printConsole, createText, setTextSize } from "skyrimPlatform";
import { getScreenResolution } from "../../view/formView";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { Mod } from "../messages_http/serverManifest";
import { logTrace } from "../../logging";
import { SettingsService } from "./settingsService";

const STATE_KEY = 'loadOrderCheckState';

interface State {
  statusTextId?: number;
};

export class LoadOrderVerificationService extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();
    this.controller.once("update", () => this.onceUpdate());
  }

  private onceUpdate() {
    this.verifyLoadOrder();
  }

  private verifyLoadOrder() {
    const settingsService = this.controller.lookupListener(SettingsService);

    this.resetText();
    const clientMods = this.getClientMods();
    this.printModOrder('Client load order:', clientMods);
    return settingsService.getServerMods()
      .then((serverMods) => {
        this.printModOrder('Server load order:', serverMods);
        // thuum docs/verbs/light-plugins.md: the engine numbers full and
        // light plugins apart, each in load order, so each kind is compared
        // with its own; a server that marks none is all full, as before
        const serverFull = serverMods.filter((m) => m.light !== true);
        const serverLight = serverMods.filter((m) => m.light === true);
        const clientLight = serverLight.length > 0 ? this.getClientLightMods() : [];
        if (serverLight.length > 0) {
          this.printModOrder('Client light plugins:', clientLight);
        }
        const fail = [
          ...this.compareMods('', clientMods, serverFull),
          ...this.compareMods('light ', clientLight, serverLight),
        ];
        if (fail.length !== 0) {
          throw new Error('Load order check failed! ' + JSON.stringify(fail));
        }
      })
      .catch((err) => {
        printConsole(err);
        if (this.sp.settings['skymp5-client']['ignoreLoadOrderMismatch']) {
          this.updateText(
            'LOAD ORDER ERROR!\nHowever, ignoring it because of ignoreLoadOrderMismatch being set.' +
            '\nExpect EVERYTHING BREAK, unless you know what you are doing.\nCheck console for details.' +
            '\nThis message will disappear after 30 seconds.',
            [255, 0, 0, 1], 30,
          );
          return;
        }
        this.updateText(
          'LOAD ORDER ERROR!\nCheck console for details.',
          [255, 0, 0, 1],
        );
      });
  };

  private getState(): State {
    if (typeof this.sp.storage[STATE_KEY] !== 'object') {
      return {};
    }
    return this.sp.storage[STATE_KEY] as State;
  };

  private setState(replacement: State) {
    const oldState = this.sp.storage[STATE_KEY] = this.getState();
    for (const [k, v] of Object.entries(replacement)) {
      (oldState as Record<string, any>)[k] = v;
    }
  };

  private resetText() {
    let { statusTextId } = this.getState();
    if (statusTextId) {
      this.sp.destroyText(statusTextId);
      statusTextId = undefined;
      this.setState({ statusTextId });
    }
  };

  private updateText(text: string, color: [number, number, number, number], clearDelay?: number) {
    const { width, height } = getScreenResolution();
    this.resetText();
    const statusTextId = createText(width / 2, height / 2, text, color);
    setTextSize(statusTextId, 0.5);
    this.setState({ statusTextId });
    if (clearDelay) {
      Utility.wait(clearDelay).then(() => this.resetText());
    }
  }

  private enumerateClientMods(getCount: (() => number), getAt: ((idx: number) => string)) {
    const result = [];
    for (let i = 0; i < getCount(); ++i) {
      const filename = getAt(i);
      const { crc32, size } = this.getFileInfoSafe(filename);
      result.push({ filename, crc32, size });
    }
    return result;
  }

  private getClientMods() {
    return this.enumerateClientMods(Game.getModCount, Game.getModName);
  };

  // SKSE's light plugin list, in the engine's light numbering order
  private getClientLightMods() {
    return this.enumerateClientMods(Game.getLightModCount, Game.getLightModName);
  };

  // The client's plugins of one kind against the server's: each of the
  // server's, by index, by name (case aside), size and CRC32. More on the
  // client is a warning for full plugins; a light plugin past the server's
  // last takes a number the server never uses.
  private compareMods(kind: string, clientMods: Mod[], serverMods: Mod[]): string[] {
    const fail: string[] = [];
    if (clientMods.length < serverMods.length) {
      fail.push(`missing ${kind}plugins: server has ${serverMods.length}, we have ${clientMods.length}`);
    }
    if (kind === '' && clientMods.length > serverMods.length) {
      this.updateText(
        'LOAD ORDER WARNING: you have more mods than server!\n(or could not receive server mod list)\nCheck console for details.',
        [255, 255, 0, 1], 5,
      );
    }
    for (let i = 0; i < Math.min(serverMods.length, clientMods.length); ++i) {
      // Need case-insensitive check for 1.6+
      if (
        clientMods[i].filename.toLowerCase() !== serverMods[i].filename.toLowerCase() ||
        clientMods[i].size !== serverMods[i].size ||
        clientMods[i].crc32 !== serverMods[i].crc32
      ) {
        fail.push(`${kind}${i}`);
        printConsole(`${i}-th ${kind}mod (numbered from 0) does not match.`);
        printConsole(`Server has ${JSON.stringify(serverMods[i])}`);
        printConsole(`We have ${JSON.stringify(clientMods[i])}`);
      }
    }
    return fail;
  }

  private printModOrder(header: string, order: Mod[]) {
    printConsole(header);
    for (const [i, mod] of Object.entries(order)) {
      printConsole(`#${i} ${JSON.stringify(mod)}`);
    }
  };

  private getFileInfoSafe(filename: string) {
    try {
      return this.sp.getFileInfo(filename);
    } catch (e) {
      const message = (e as Record<string, unknown>).message;

      if (typeof message === "string" && message.includes('is not a valid argument')) {
        logTrace(this, `Failed to get file info for`, filename);
        return { crc32: 0, size: 0 };
      } else {
        throw e;
      }
    }
  }
}
