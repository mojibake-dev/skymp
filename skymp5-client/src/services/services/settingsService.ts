import { HttpClient, HttpHeaders, HttpResponse, printConsole, Utility } from "skyrimPlatform";
import { AuthService } from "./authService";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { Mod, ServerManifest } from "../messages_http/serverManifest";
import { TimersService } from "./timersService";
import { logTrace } from "../../logging";

interface IHttpClientWithCallback {
  get(path: string, options?: { headers?: HttpHeaders }): Promise<HttpResponse>;
  post(path: string, options: { body: string, contentType: string, headers?: HttpHeaders }): Promise<HttpResponse>;

  get(path: string, options: { headers?: HttpHeaders } | undefined, callback: (response: HttpResponse) => void): void;
}

export interface TargetPeer {
  host: string;
  port: number;
  publicKeys?: Record<string, string | undefined>;
}

export type TargetPeerCallback = (targetPeer: TargetPeer) => void;

export class SettingsService extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();
  }

  public getServerMasterKey() {
    let masterKey = this.sp.settings["skymp5-client"]["server-master-key"];
    if (!masterKey) {
      masterKey = this.sp.settings["skymp5-client"]["master-key"];
    }
    if (!masterKey) {
      masterKey = this.sp.settings["skymp5-client"]["server-ip"] + ":" + this.sp.settings["skymp5-client"]["server-port"];
    }
    return masterKey;
  }

  public getMasterUrl() {
    return this.normalizeUrl((this.sp.settings["skymp5-client"]["master"] as string) || "https://gateway.skymp.net");
  }

  public makeMasterApiClient(): IHttpClientWithCallback {
    const masterApiBaseUrl = this.getMasterUrl();
    return new HttpClient(masterApiBaseUrl) as IHttpClientWithCallback;
  }

  public getTargetPeer(callback?: TargetPeerCallback): { targetPeerCached: TargetPeer | null } {
    if (this.targetPeerCache) {
      callback?.(this.targetPeerCache);
      return { targetPeerCached: this.targetPeerCache };
    }
    this.getTargetPeerImpl((targetPeer) => {
      this.targetPeerCache = targetPeer;
      callback?.(targetPeer);
    });
    return { targetPeerCached: null };
  }

  // have to use callbacks here: promises don't work in the main menu
  private getTargetPeerImpl(callback: TargetPeerCallback) {
    const masterApiClient = this.makeMasterApiClient();
    const masterKey = this.getServerMasterKey();

    const serverInfoRequestTimeoutMs = 5000;
    const defaultPeer: TargetPeer = {
      host: this.sp.settings['skymp5-client']['server-ip'] as string,
      port: this.sp.settings['skymp5-client']['server-port'] as number,
      publicKeys: this.sp.settings['skymp5-client']['server-public-keys'] as Record<string, string | undefined> | undefined,
    };

    let resolved = false;

    const states = {
      start: () => {
        try {
          if (this.sp.settings['skymp5-client']['server-info-ignore'] as boolean) {
            logTrace(this, 'Skipping serverinfo request due to server-info-ignore in config');
            states.resolve(defaultPeer);
            return;
          }
          this.controller.lookupListener(TimersService).setTimeout(
            () => states.reject(new Error('getTargetPeer: serverinfo request timed out')),
            serverInfoRequestTimeoutMs,
          );

          let headers: HttpHeaders = {};
          let session = this.controller.lookupListener(AuthService).readAuthDataFromDisk()?.session;
          if (session) {
            headers['X-Session'] = session;
          }

          masterApiClient.get(
            `/api/servers/${masterKey}/serverinfo`, { headers },
            states.handleResponse,
          );
        } catch (e) {
          states.reject(e);
        }
      },
      handleResponse: (res: HttpResponse) => {
        try {
          if (res.status !== 200) {
            throw new Error(`status ${res.status}`);
          }
          states.resolve(JSON.parse(res.body));
        } catch (e) {
          states.reject(e);
        }
      },

      resolve: (targetPeer: TargetPeer) => {
        if (resolved) {
          return;
        }
        resolved = true;

        logTrace(this, `Resolved target peer`, targetPeer);

        const enrichedTargetPeer = { ...targetPeer };
        enrichedTargetPeer.publicKeys = { ...defaultPeer.publicKeys, ...targetPeer.publicKeys };

        logTrace(this, `Enriched target peer`, enrichedTargetPeer);

        callback(enrichedTargetPeer);
      },
      reject: (err: unknown) => {
        if (resolved) {
          return;
        }
        resolved = true;

        logTrace(this, `Server info request failed, falling back to`, defaultPeer, `; error:`, err);
        callback(defaultPeer);
      },
    };

    states.start();
  }

  // The server's files (skymp5-server manifestGen.ts: each plugin of its
  // load order, a light one flagged, and any archive beside it, with size
  // and CRC32) from the master API's copy of the server's manifest.json, in
  // five attempts. thuum x-quest-runs 20261110-071422's read (fault 3): with
  // ownServerFirst the game server's own manifest.json is read first, from
  // its resources port on the host this client connects to
  // (getOwnServerResourcesUrl), and the master stays the fallback as before.
  // The lab's seats reach no master (both seats' five attempts failed in
  // that read, "Could not establish connection"), while sky-srv serves the
  // manifest on 3000 (200, version 1, 91 plugins, read there 2026-11-10).
  // onMiss hears why an answer is an empty list. LoadOrderVerificationService
  // keeps the master alone: given the server's list it would compare every
  // served plugin on a lab seat for the first time, and the two whose names
  // hold an ampersand read size 0 and CRC32 0 here (getFileInfo refuses
  // them: Dwemer Goggles & Scouter.esp at 0x22, Cloaks&Capes.esp at 0x26), a
  // LOAD ORDER ERROR left on screen; the other 46 served full plugins match
  // the server's by name, size and CRC32
  public async getServerMods(ownServerFirst = false, onMiss?: (reason: string) => void): Promise<Mod[]> {
    let ownMiss = '';
    if (ownServerFirst) {
      let where = "the server's own manifest.json";
      try {
        const base = this.getOwnServerResourcesUrl();
        where = `${base}/manifest.json`;
        const res = await new HttpClient(base).get('/manifest.json');
        if (res.status != 200) {
          throw new Error(`status code ${res.status}, error ${res.error}`);
        }
        const manifest = JSON.parse(res.body) as ServerManifest;
        const files = Array.isArray(manifest.mods) ? manifest.mods.length : 0;
        if (manifest.versionMajor === 1 && files > 0) {
          return manifest.mods;
        }
        throw new Error(`manifest version ${manifest.versionMajor} with ${files} file(s)`);
      } catch (e) {
        ownMiss = `${where}: ${e}`;
        printConsole(`The server's own manifest is unavailable (${ownMiss}); trying the master`);
      }
    }
    const miss = (master: string) => onMiss?.(ownMiss ? `${ownMiss}; the master: ${master}` : `the master: ${master}`);

    const masterApiClient = this.makeMasterApiClient();

    const masterKey = this.getServerMasterKey();
    printConsole(masterKey);

    let masterError = '';
    for (let attempt = 0; attempt < 5; ++attempt) {
      try {
        printConsole(`Trying to get server mods, attempt ${attempt}`);
        const res = await masterApiClient.get(`/api/servers/${masterKey}/manifest.json`);
        if (res.status != 200) {
          throw new Error(`status code ${res.status}, error ${res.error}`);
        }
        const manifest = JSON.parse(res.body) as ServerManifest;
        if (manifest.versionMajor !== 1) {
          printConsole(`server manifest version is ${manifest.versionMajor}, we expect 1`);
          miss(`manifest version ${manifest.versionMajor}, 1 expected`);
          return [];
        }
        return manifest.mods;
      } catch (e) {
        printConsole(`Request/parse error: ${e}`);
        masterError = `${e}`;
        await Utility.wait(0.1 + Math.random());
      }
    }

    miss(`5 attempts, the last ${masterError}`);
    return [];
  };

  // The game server's resources folder (skymp5-server ui.ts:102: game port
  // 7777's is 3000, any other game port's is that port plus 1; ui.ts:88
  // serves data/ there, where manifestGen writes manifest.json), on the host
  // this client connects to: the resolved target peer, else the settings'
  // server address (a seat that skips the server info connects to that)
  private getOwnServerResourcesUrl(): string {
    const peer = this.targetPeerCache;
    const host: unknown = peer ? peer.host : this.sp.settings['skymp5-client']['server-ip'];
    const port = Number(peer ? peer.port : this.sp.settings['skymp5-client']['server-port']);
    if (typeof host !== 'string' || host.length === 0 || !Number.isInteger(port) || port <= 0) {
      throw new Error(`no server address (server-ip ${String(host)}, server-port ${String(port)})`);
    }
    return `http://${host}:${port === 7777 ? 3000 : port + 1}`;
  }

  private normalizeUrl(url: string) {
    if (url.endsWith('/')) {
      return url.slice(0, url.length - 1);
    }
    return url;
  };

  private targetPeerCache: TargetPeer | null = null;
}
