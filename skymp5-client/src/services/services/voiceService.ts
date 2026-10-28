import { isPuppet } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { ActorSpeechMessage } from "../messages/actorSpeechMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";
import { ConnectionMessage } from "../events/connectionMessage";
import { HostService } from "./hostService";
import { RemoteServer } from "./remoteServer";
import { localIdToRemoteId, remoteIdToLocalId } from "../../view/worldViewMisc";
import { logTrace } from "../../logging";

// thuum docs/verbs/npc-stream.md (the voice slice, ADR-050): the lines the
// actors this game simulates speak (greetings, idle chatter, scenes: every
// topic info the engine begins on them; the fork's Skyrim Platform raises
// topicInfo for a speaker in the hosted set, the begin alone, filtered in
// native) go to the server as ActorSpeech under the actor's idx, unreliable;
// the server relays them to the actor's other listeners, and here a puppet
// says the same info through the engine's own say (sayTopicInfo: the lip
// sync, the talking idle and the begin event are the engine's, the line's
// end the puppet detour's). TES3MP's ActorSpeech shape. Under the npcStream
// switch. HYPOTHESIS until P7: the puppet's line starts (the native answers
// 1) and ends (the detour's "line ended" line).
export class VoiceService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        // thuum docs/verbs/npc-stream.md (the voice slice): the client switch
        // npcVoice; on by default, off keeps the host's lines unsent and a
        // puppet's say unplayed (D27-B: the voice rides a build only when P7
        // is green; a merge that carries this code with a red P7 flips the
        // default off)
        this.enabled = this.sp.settings["skymp5-client"]["npcVoice"] !== false;
        if (!this.enabled) {
            logTrace(this, "npcVoice is off in settings: no spoken line goes out and no puppet says one");
        }
        this.controller.on("topicInfo", (e) => this.onTopicInfo(e));
        this.controller.emitter.on("actorSpeechMessage", (e) => this.onActorSpeech(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // the event's context allows no game call; the id is mapped on the update
    private onTopicInfo(e: { speakerId: number; topicInfo: number; topic: number }) {
        if (!this.enabled) {
            return;
        }
        if (!this.controller.lookupListener(HostService).isStreamEnabled()) {
            return;
        }
        if (!e.topicInfo || !e.topic) {
            return;
        }
        // the actor's last line on this engine, for the dialogue claim's
        // release (TalkService: the post-dialogue line came 3 s after the
        // menu's close, on the grace's edge; playtest 37, 22:04:00Z)
        this.lastSpokeAt.set(e.speakerId, Date.now());
        this.controller.once("update", () => {
            const remoteId = localIdToRemoteId(e.speakerId);
            if (!remoteId) {
                return;
            }
            const message: MessageWithRefrId<ActorSpeechMessage> = {
                t: MsgType.ActorSpeech,
                topic: e.topic,
                info: e.topicInfo,
                _refrId: remoteId,
            };
            // playtest 37 (2026-10-28 20:54Z): two lines fired 24 ms apart on the
            // host (a response, then a greeting) reached the watcher in the other
            // order over the unreliable channel, and the greeting cut the response;
            // the newest line wins on both engines, so the order must be the host's
            this.controller.emitter.emit("sendMessageWithRefrId", { message, reliability: "reliable" });
            logTrace(this, `speech ${remoteId.toString(16)}: info ${e.topicInfo.toString(16)} topic ${e.topic.toString(16)}`);
        });
    }

    // a relayed line: the puppet of that actor says it (never an actor this
    // game hosts, Rule A; never a figure or a dead actor)
    private onActorSpeech(event: ConnectionMessage<ActorSpeechMessage>) {
        if (!this.enabled) {
            return;
        }
        const msg = event.message;
        this.controller.once("update", () => {
            const world = this.controller.lookupListener(RemoteServer).getWorldModel();
            const form = world.forms.find((f) => f !== undefined && f.idx === msg.idx);
            if (!form || form.refrId === undefined) {
                return;
            }
            const localId = remoteIdToLocalId(form.refrId);
            if (!localId || localId === 0x14) {
                return;
            }
            if (this.controller.lookupListener(HostService).isMineLocal(localId)) {
                return;
            }
            if (!isPuppet(localId)) {
                // playtest 37 (2026-10-28, Eli: "delphines bark and when rotfern
                // went into the bar, orgnars bark" never crossed): a greeting
                // fires on the host the moment the entering player's figure
                // appears, and its relay reaches the entering seat while its
                // engine still loads the cell, before the speaker is a puppet
                // here; the line was dropped on this test. It waits arrivalMs
                // for the puppet instead (TES3MP says a late line late)
                this.arrivals.set(localId, { info: msg.info, since: Date.now() });
                logTrace(this, `say ${localId.toString(16)} info ${msg.info.toString(16)} waits for the puppet`);
                return;
            }
            this.ask(localId, msg.info);
        });
    }

    private ask(localId: number, info: number): boolean {
        const actor = this.sp.Actor.from(this.sp.Game.getFormEx(localId));
        if (!actor || actor.isDead()) {
            return false;
        }
        if (!this.sp.sayTopicInfo(localId, info)) {
            return false;
        }
        this.pending.set(localId, { info, since: Date.now() });
        logTrace(this, `say ${localId.toString(16)} info ${info.toString(16)} asked`);
        return true;
    }

    // the native's answer, read on the updates after the ask
    private onUpdate() {
        if (this.arrivals.size > 0) {
            const now = Date.now();
            for (const [localId, line] of Array.from(this.arrivals.entries())) {
                if (now - line.since > VoiceService.arrivalMs) {
                    this.arrivals.delete(localId);
                    logTrace(this, `say ${localId.toString(16)} info ${line.info.toString(16)} -> no puppet within ${VoiceService.arrivalMs} ms, dropped`);
                    continue;
                }
                if (this.controller.lookupListener(HostService).isMineLocal(localId)) {
                    this.arrivals.delete(localId);
                    continue;
                }
                if (isPuppet(localId)) {
                    this.arrivals.delete(localId);
                    this.ask(localId, line.info);
                }
            }
        }
        if (this.pending.size === 0) {
            return;
        }
        const now = Date.now();
        for (const [localId, ask] of Array.from(this.pending.entries())) {
            const result = this.sp.sayTopicInfoResult(localId);
            if (result >= 0) {
                const word = result === 1 ? "started" : result === 3 ? "played" : result === 5 ? "inaudible" : result === 4 ? "deferred" : result === 2 ? "refused" : "no actor or info";
                logTrace(this, `say ${localId.toString(16)} info ${ask.info.toString(16)} -> ${word}`);
                this.pending.delete(localId);
            } else if (now - ask.since > VoiceService.answerMs) {
                logTrace(this, `say ${localId.toString(16)} info ${ask.info.toString(16)} -> no answer in ${VoiceService.answerMs} ms`);
                this.pending.delete(localId);
            }
        }
    }

    // when an actor last began a line on this engine (ms), by local id
    lastSpoke(localId: number): number {
        return this.lastSpokeAt.get(localId) ?? 0;
    }

    private readonly lastSpokeAt = new Map<number, number>();
    private readonly pending = new Map<number, { info: number; since: number }>();
    // lines that arrived before their speaker was a puppet here (a cell still loading)
    private readonly arrivals = new Map<number, { info: number; since: number }>();
    private static readonly answerMs = 2000;
    private static readonly arrivalMs = 6000;
    private readonly enabled: boolean;
}
