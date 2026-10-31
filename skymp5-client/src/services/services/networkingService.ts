import { logTrace, logError, logLab } from "../../logging";
import { NeverError } from "../../lib/errors";
import { MsgType } from "../../messages";
import { SendMessageEvent } from "../events/sendMessageEvent";
import { SendMessageWithRefrIdEvent } from "../events/sendMessageWithRefrIdEvent";
import { AnyMessage } from "../messages/anyMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";
import { SendRawMessageEvent } from "../events/sendRawMessageEvent";

export class NetworkingService extends ClientListener {
  private wrongPlayerSlotLogged = false;
  constructor(private sp: Sp, private controller: CombinedController) {
    super();
    this.controller.on("tick", () => this.onTick());

    this.controller.emitter.on("sendMessage", (e) => this.onSendMessage(e));
    this.controller.emitter.on("sendRawMessage", (e) => this.onSendRawMessage(e));
    this.controller.emitter.on("sendMessageWithRefrId", (e) => this.onSendMessageWithRefrId(e));
  }

  private onSendMessage(e: SendMessageEvent<AnyMessage>) {
    this.sp.mpClientPlugin.send(JSON.stringify(e.message), this.isReliable(e.reliability));
  }

  private onSendRawMessage(e: SendRawMessageEvent) {
    // @ts-expect-error
    this.sp.mpClientPlugin.sendRaw(e.rawMessage, e.rawMessage.byteLength, this.isReliable(e.reliability));
  }

  private onSendMessageWithRefrId(e: SendMessageWithRefrIdEvent<AnyMessage>) {
    const refrId = e.message._refrId;

    const remoteServer = this.controller.lookupListener(RemoteServer);

    const idxInModel = refrId
      ? remoteServer.getWorldModel().forms.findIndex((f) => f && f.refrId === refrId)
      : remoteServer.getWorldModel().playerCharacterFormIdx;

    // fixes "can't get property idx of null or undefined"
    const target = remoteServer.getWorldModel().forms[idxInModel];
    if (!target) {
      return;
    }
    // thuum docs/verbs/npc-puppet.md: a message about the player goes out
    // under the player's own form alone. With game-file actors creating
    // models at login, the player's slot can hold another actor's model for
    // a moment (x-npc-landing-trace 20261022-054932: the player's movement
    // streamed under a deer's index from login to the grant, then moved the
    // deer's record to the player), so the slot is verified, not trusted
    if (!refrId && target.isMyClone !== true) {
      if (!this.wrongPlayerSlotLogged) {
        this.wrongPlayerSlotLogged = true;
        logError(this, `player message dropped: slot ${idxInModel} holds ${target.refrId?.toString(16)}, not the player`);
      }
      return;
    }
    this.wrongPlayerSlotLogged = false;

    // @ts-ignore
    e.message.idx = remoteServer.getWorldModel().forms[idxInModel].idx;

    delete e.message._refrId;

    this.sp.mpClientPlugin.send(JSON.stringify(e.message), this.isReliable(e.reliability));
  }

  connect(hostName: string, port: number) {
    this.serverAddress = { hostName, port };
    this.createClientSafe();
  }

  reconnect() {
    this.createClientSafe();
  }

  close() {
    this.sp.mpClientPlugin.destroyClient();
  }

  isConnected() {
    return this.sp.mpClientPlugin.isConnected();
  }

  private onTick() {
    this.sp.mpClientPlugin.tick((packetType, rawContent, error) => {
      switch (packetType) {
        case "connectionAccepted":
          this.controller.emitter.emit("connectionAccepted", {});
          break;
        case "connectionDenied":
          this.controller.emitter.emit("connectionDenied", { error });
          this.reconnect();
          break;
        case "connectionFailed":
          this.controller.emitter.emit("connectionFailed", {});
          this.reconnect();
          break;
        case "disconnect":
          this.controller.emitter.emit("connectionDisconnect", {});
          this.reconnect();
          break;
        case "message":
          // TODO: in theory can be empty jsonContent and non-empty error

          let msgAny: AnyMessage;
          
          if (rawContent === null) {
            msgAny = {} as AnyMessage;
            logError(this, "null rawContent");
          } else if ((new Uint8Array(rawContent as unknown as ArrayBuffer)[0]) === 0x7b) {
            // assume json
            msgAny = JSON.parse(this.sp.decodeUtf8(rawContent as unknown as ArrayBuffer));
          } else {
            // assume raw
            const event = { rawContent: rawContent as unknown as ArrayBuffer};
            return this.controller.emitter.emit("anyRawMessage", event);
          }

          if (msgAny.t === MsgType.OpenContainer) {
            const event = { message: msgAny };
            this.controller.emitter.emit("openContainerMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.UpdateMovement) {
            const event = { message: msgAny };
            this.controller.emitter.emit("updateMovementMessage", event)
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.UpdateAnimation) {
            const event = { message: msgAny };
            this.controller.emitter.emit("updateAnimationMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.UpdateEquipment) {
            const event = { message: msgAny };
            this.controller.emitter.emit("updateEquipmentMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.ChangeValues) {
            const event = { message: msgAny };
            this.controller.emitter.emit("changeValuesMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.SpellCast) {
            const event = { message: msgAny };
            this.controller.emitter.emit("spellCastMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.UpdateAnimVariables) {
            const event = { message: msgAny };
            this.controller.emitter.emit("updateAnimVariablesMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.UpdateAppearance) {
            const event = { message: msgAny };
            this.controller.emitter.emit("updateAppearanceMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.Teleport) {
            const event = { message: msgAny };
            // thuum docs/verbs/npc-handoff.md D6: the viewer's Teleport for a
            // hosted actor's door (x-handoff-door 20261024-013234 read no
            // handler line on the viewer; this names arrival apart from
            // handling)
            logLab("verb:npc-handoff", `Teleport received for idx ${(msgAny as { idx?: number }).idx} into ${((msgAny as { worldOrCell?: number }).worldOrCell ?? 0).toString(16)}`);
            this.controller.emitter.emit("teleportMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.UpdateProperty) {
            const event = { message: msgAny };
            this.controller.emitter.emit("updatePropertyMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.DeathStateContainer) {
            const event = { message: msgAny };
            this.controller.emitter.emit("deathStateContainerMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.CreateActor) {
            const event = { message: msgAny };
            this.controller.emitter.emit("createActorMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.CustomPacket) {
            const event = { message: msgAny };
            this.controller.emitter.emit("customPacketMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.DestroyActor) {
            const event = { message: msgAny };
            this.controller.emitter.emit("destroyActorMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.HostStart) {
            const event = { message: msgAny };
            this.controller.emitter.emit("hostStartMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.HostStop) {
            const event = { message: msgAny };
            this.controller.emitter.emit("hostStopMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.SetInventory) {
            const event = { message: msgAny };
            this.controller.emitter.emit("setInventoryMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.SetRaceMenuOpen) {
            const event = { message: msgAny };
            this.controller.emitter.emit("setRaceMenuOpenMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.SpSnippet) {
            const event = { message: msgAny };
            this.controller.emitter.emit("spSnippetMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.UpdateGamemodeData) {
            const event = { message: msgAny };
            this.controller.emitter.emit("updateGamemodeDataMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.Teleport2) {
            const event = { message: msgAny };
            this.controller.emitter.emit("teleportMessage2", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.SetGameTime) {
            const event = { message: msgAny };
            this.controller.emitter.emit("setGameTimeMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.Favorites) {
            const event = { message: msgAny };
            this.controller.emitter.emit("favoritesMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.RaceMenuPreset) {
            const event = { message: msgAny };
            this.controller.emitter.emit("raceMenuPresetMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.ActorValues) {
            const event = { message: msgAny };
            this.controller.emitter.emit("actorValuesMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.ConsoleOutput) {
            const event = { message: msgAny };
            this.controller.emitter.emit("consoleOutputMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.ArrowShot) {
            const event = { message: msgAny };
            this.controller.emitter.emit("arrowShotMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.MagicEffects) {
            const event = { message: msgAny };
            this.controller.emitter.emit("magicEffectsMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.UpdateAction) {
            const event = { message: msgAny };
            this.controller.emitter.emit("updateActionMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.ActorSpeech) {
            const event = { message: msgAny };
            this.controller.emitter.emit("actorSpeechMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.HostedActorState) {
            const event = { message: msgAny };
            this.controller.emitter.emit("hostedActorStateMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.DamageCause) {
            const event = { message: msgAny };
            this.controller.emitter.emit("damageCauseMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.PlayerGlobals) {
            const event = { message: msgAny };
            this.controller.emitter.emit("playerGlobalsMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.QuestStage) {
            const event = { message: msgAny };
            this.controller.emitter.emit("questStageMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.PropTransform) {
            const event = { message: msgAny };
            this.controller.emitter.emit("propTransformMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.PropOwner) {
            const event = { message: msgAny };
            this.controller.emitter.emit("propOwnerMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.PlacedRequest) {
            const event = { message: msgAny };
            this.controller.emitter.emit("placedRequestMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.ActorJump) {
            const event = { message: msgAny };
            this.controller.emitter.emit("actorJumpMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.HostGrant) {
            const event = { message: msgAny };
            this.controller.emitter.emit("hostGrantMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.HostRelease) {
            const event = { message: msgAny };
            this.controller.emitter.emit("hostReleaseMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else if (msgAny.t === MsgType.ActorFate) {
            const event = { message: msgAny };
            this.controller.emitter.emit("actorFateMessage", event);
            this.controller.emitter.emit("anyMessage", event);
          } else {
            // throw new NeverError(msgAny);
            throw new Error("Unhandled MsgType " + msgAny.t);
          }
          break;
      }
    });
  }

  private createClientSafe() {
    const { hostName, port } = this.serverAddress;

    logTrace(this, "createClientSafe " + hostName + ":" + port);

    if (this.serverAddress.hostName !== "" && this.serverAddress.port !== 0) {
      this.sp.mpClientPlugin.createClient(hostName, port);
      // thuum docs/research/m3/decisions-after-playtest-34.md D24 (B): the
      // game's quit ends the process right after the client's teardown
      // instead of the engine's shutdown, which crashes at every quit (two
      // SKSE plugins hold the Loading Menu past the UI's end;
      // cell-host.md); quitFastExit false in skymp5-client-settings.txt
      // keeps the engine's own shutdown for a read of it
      try {
        const fastExit = this.sp.settings["skymp5-client"]["quitFastExit"] !== false;
        // typed by duck: the client's copy of the platform's declarations
        // (node_modules/@skyrim-platform) lags the fork's Definitions.txt
        const plugin = this.sp.mpClientPlugin as unknown as { setQuitFastExit?: (fastExit: boolean) => void };
        if (typeof plugin.setQuitFastExit === "function") {
          plugin.setQuitFastExit(fastExit);
        } else {
          logError(this, "setQuitFastExit missing: an older platform dist, the engine's shutdown runs at the quit");
        }
      } catch (e) {
        logError(this, "setQuitFastExit failed", String(e));
      }
    } else {
      logError(this, "createClientSafe failed");
    }
  }

  private get serverAddress(): { hostName: string, port: number } {
    const res: unknown = this.sp.storage["serverAddress"];
    if (typeof res === "object") {
      const result = res as { hostName: string, port: number };
      if (typeof result.hostName === "string" && typeof result.port === "number") {
        return result;
      }
    }
    return { hostName: "", port: 0 };
  }

  private set serverAddress(newValue: { hostName: string, port: number }) {
    this.sp.storage["serverAddress"] = newValue;
  }

  private isReliable(reliability: "reliable" | "unreliable") {
    switch (reliability) {
      case "reliable":
        return true;
      case "unreliable":
        return false;
      default:
        throw new NeverError(reliability);
    }
  }
};
