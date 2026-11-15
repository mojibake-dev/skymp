import { Actor, Package, Quest } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { CLAIM_LEAD, LEADS_PER_ACTOR, LeadReading, claimMessage, claimOutbox, isRecordId, judgeLead, opensLead } from "./claimRules";

const TAG = "verb:npc-talk";
// the poll, HostedStateService's
const POLL_MS = 500;
// an actor whose quest's package runs but which does not yet open a lead (not
// this seat's, no alias naming her) is read again this often, not every poll
const RECHECK_MS = 2000;
// a quest's aliases read at most (MS13 has a dozen)
const ALIASES_MAX = 256;
const PACKAGES_MAX = 4096;

// alias: the index (getNthAlias's) of the alias that named her last, read
// first at the next poll
type Lead = { localId: number; remoteId: number; quest: number; pkg: number; since: number; alias: { index: number } };
type Running = { pkg: number; quest: number; checkedAt: number; alias: { index: number } };

const hex = (n: number) => n.toString(16);

// thuum docs/DECISIONS.md ADR-073 step 5a, the client half (decision 6's lead
// as D-53-4 amends it: "A quest's own package on that quest's NPC is the
// quest's claim, until the quest releases her, stops, or she dies"; D-PO2 A,
// the game running a quest's lead claims the NPC until it ends, across cells
// and surviving a grant). R-C2 (lab/.cache/drafts/reads-53): Camilla 0x13488
// walked out on MS13's travel package 0x2e4cc under rotfern's game, and
// Ariadne's grant of the exterior took her back inside.
//
// The feed is HostedStateService's package events (packageStart and
// packageChange, the one crossing): each package's owning quest, its QNAM
// (Package.getOwningQuest, TESPackage::ownerQuest, CommonLibSSE-NG
// include/RE/T/TESPackage.h:310), read once per package. At its poll (500 ms,
// or the update after a quest's package starts) the service opens a lead on
// an actor whose running package is its quest's own when this seat simulates
// her (HostService.isMineLocal: the word names this seat), she is alive, the
// quest runs here and one of the quest's reference aliases names her in this
// seat's engine (Quest.getNumAliases and getNthAlias, then
// ReferenceAlias.getReference; the server's own record reaches a client only
// as relays of reported fills, and no seat reports an ALUA or ALFR fill), and
// sends ActorClaim {purpose 1, quest, package} through the claims' outbox.
// The server checks the records and the simulator (E_CLAIM_NOT_SIMULATOR for
// a seat that does not run her) and answers no word for a holder that already
// simulates her. A package that names no quest (MS13's alias stack carries
// 0x63b2a, the argument scene's) neither opens nor ends a lead.
//
// A held lead is read at each poll: forgotten at her death or at a word naming
// another seat (the server ended it, or refused it), released ({claim false,
// purpose 1, quest, package}) when the quest stops here or no alias of it
// names her here. A new connection forgets every lead (the server ended them
// at the departure) and the poll opens them again from the packages kept here.
//
// HYPOTHESIS H-L2: getOwningQuest answers the winning record's ownerQuest
// (USSEP's QNAM on 0xced81 included) and the package events fire for 0x2e4cc
// (hostedStateService.ts's H3). H-L3: getNthAlias names an ALUA fill at
// runtime (MS13's alias 9 by unique actor). H-L1 (the server's): the holder
// streams her with the exterior unloaded.
export class LeadClaimService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
    }

    // HostedStateService.onPackage's feed, in the update's context (the
    // package events are posted as update tasks): the package the engine now
    // runs on the actor. A package of no quest drops the actor from the
    // running set; a lead held on her stays
    onPackage(localId: number, pkg: Package | null): void {
        const pkgId = pkg ? pkg.getFormID() : 0;
        const quest = pkg ? this.questOf(pkgId, pkg) : 0;
        if (quest === 0) {
            this.running.delete(localId);
            return;
        }
        this.running.set(localId, { pkg: pkgId, quest, checkedAt: 0, alias: { index: -1 } });
        this.nextPoll = 0;
    }

    private onUpdate(): void {
        if (this.held.size === 0 && this.running.size === 0 && claimOutbox.size === 0) {
            return;
        }
        const now = Date.now();
        if (now < this.nextPoll) {
            return;
        }
        this.nextPoll = now + POLL_MS;
        const host = this.controller.lookupListener(HostService);
        for (const [key, lead] of Array.from(this.held.entries())) {
            const verdict = judgeLead(this.reading(lead.localId, lead.quest, host, lead.alias));
            if (verdict.act === "keep") {
                continue;
            }
            this.held.delete(key);
            const held = ((now - lead.since) / 1000).toFixed(1);
            if (verdict.act === "release") {
                const message = claimMessage(lead.remoteId, false, { purpose: CLAIM_LEAD, quest: lead.quest, package: lead.pkg });
                claimOutbox.push(message, `lead of ${hex(lead.quest)} on ${hex(lead.remoteId)} released at ${verdict.why} (package ${hex(lead.pkg)}, held ${held} s)`);
            } else {
                logLab(TAG, `lead of ${hex(lead.quest)} on ${hex(lead.remoteId)} forgotten at ${verdict.why} (package ${hex(lead.pkg)}, held ${held} s)`);
            }
        }
        for (const [localId, run] of Array.from(this.running.entries())) {
            if (this.held.has(LeadClaimService.key(localId, run.quest)) || now - run.checkedAt < RECHECK_MS) {
                continue;
            }
            run.checkedAt = now;
            const reading = this.reading(localId, run.quest, host, run.alias);
            if (!reading.present()) {
                this.running.delete(localId);
                continue;
            }
            if (!opensLead(reading)) {
                continue;
            }
            const remoteId = localIdToRemoteId(localId);
            if (!remoteId || !isRecordId(run.quest) || !isRecordId(run.pkg)) {
                continue;
            }
            if (this.leadsOf(localId) >= LEADS_PER_ACTOR) {
                logTrace(this, `lead of ${hex(run.quest)} on ${hex(remoteId)} not asked: this seat holds ${LEADS_PER_ACTOR} quests' leads on her already`);
                continue;
            }
            this.held.set(LeadClaimService.key(localId, run.quest), { localId, remoteId, quest: run.quest, pkg: run.pkg, since: now, alias: { index: run.alias.index } });
            const message = claimMessage(remoteId, true, { purpose: CLAIM_LEAD, quest: run.quest, package: run.pkg });
            claimOutbox.push(message, `lead of ${hex(run.quest)} claimed on ${hex(remoteId)} (local ${hex(localId)}): package ${hex(run.pkg)}, the quest's own, runs on her here, an alias of the quest names her and this seat simulates her`);
        }
        this.flush(now);
    }

    // the outbox's messages the bucket lets go now, each with its line
    private flush(now: number): void {
        for (const { message, line } of claimOutbox.drain(now)) {
            this.controller.emitter.emit("sendMessageWithRefrId", { message, reliability: "reliable" });
            logLab(TAG, line);
        }
    }

    // a new connection (connectionAccepted, ahead of all its messages): the
    // server ended this seat's leads at its departure, so none is held; the
    // running packages stay and the poll opens their leads again once the
    // word names this seat
    private onConnectionAccepted(): void {
        const n = this.held.size;
        this.held.clear();
        this.running.forEach((run) => {
            run.checkedAt = 0;
        });
        claimOutbox.clear();
        if (n > 0) {
            logLab(TAG, `a new connection: ${n} lead(s) forgotten (the server ended them at the departure), asked again from the packages running here`);
        }
    }

    private reading(localId: number, questId: number, host: HostService, alias: { index: number }): LeadReading {
        let actor: Actor | null | undefined;
        let quest: Quest | null | undefined;
        const actorOf = (): Actor | null => {
            if (actor === undefined) {
                actor = this.sp.Actor.from(this.sp.Game.getFormEx(localId));
            }
            return actor;
        };
        const questOf = (): Quest | null => {
            if (quest === undefined) {
                quest = this.sp.Quest.from(this.sp.Game.getFormEx(questId));
            }
            return quest;
        };
        return {
            present: () => actorOf() !== null,
            dead: () => {
                const a = actorOf();
                return a === null || a.isDead();
            },
            mine: () => host.isMineLocal(localId),
            questRunning: () => {
                const q = questOf();
                return q !== null && (q.isRunning() || q.isStarting());
            },
            aliasNames: () => {
                const q = questOf();
                return q !== null && this.aliasNames(q, localId, alias);
            },
        };
    }

    // whether one of the quest's reference aliases holds the actor in this
    // seat's engine (a location alias answers no reference alias): the alias
    // that named her last first, then every alias, the one found kept
    private aliasNames(quest: Quest, localId: number, last: { index: number }): boolean {
        const holds = (i: number) => {
            const alias = this.sp.ReferenceAlias.from(quest.getNthAlias(i));
            const ref = alias ? alias.getReference() : null;
            return ref !== null && ref.getFormID() === localId;
        };
        if (last.index >= 0 && holds(last.index)) {
            return true;
        }
        const n = Math.min(quest.getNumAliases(), ALIASES_MAX);
        for (let i = 0; i < n; ++i) {
            if (i !== last.index && holds(i)) {
                last.index = i;
                return true;
            }
        }
        last.index = -1;
        return false;
    }

    // a package's owning quest's form id (0 for none), read once per package
    private questOf(pkgId: number, pkg: Package): number {
        const known = this.packageQuest.get(pkgId);
        if (known !== undefined) {
            return known;
        }
        const owner = pkg.getOwningQuest();
        const quest = owner ? owner.getFormID() : 0;
        if (this.packageQuest.size >= PACKAGES_MAX) {
            this.packageQuest.clear();
        }
        this.packageQuest.set(pkgId, quest);
        return quest;
    }

    private leadsOf(localId: number): number {
        let n = 0;
        this.held.forEach((lead) => {
            if (lead.localId === localId) {
                n++;
            }
        });
        return n;
    }

    private static key(localId: number, quest: number): string {
        return `${localId}:${quest}`;
    }

    // the leads this seat asked, by actor and quest
    private readonly held = new Map<string, Lead>();
    // the actors whose running package is a quest's own, by local id
    private readonly running = new Map<number, Running>();
    private readonly packageQuest = new Map<number, number>();
    private nextPoll = 0;
}
