# PlayerBots — Milestone Roadmap

**North Star:** `docs/playerbots/PLAYERBOTS_VISION.md`
**M0 commit:** `e8dad3771`
**Checkbox-regel:** En milstolpe markeras färdig först när kod, tester, review, commit, push och rapport är klara.

## Gemensam Definition of Done för varje milstolpe

En milstolpe får markeras `[x]` först när:

- [ ] accepterade krav är implementerade;
- [ ] inga otillåtna direkta gameplay-stateändringar används;
- [ ] fokuserade unit-tester passerar;
- [ ] fokuserade databasbackade integrationstester passerar där det är relevant;
- [ ] `git diff --check` passerar;
- [ ] vanlig player behavior har regressionstäckning;
- [ ] slutlig read-only review hittar ingen blocker;
- [ ] ändringarna är committade och pushade;
- [ ] full Codex-handoff har skrivits till rapportfil;
- [ ] roadmapens status och evidens har uppdaterats.

---

## [x] M0 — Headless Player Lifecycle

### Mål

Skapa en säker serverkontrollerad `Player` utan nätverksklient med korrekt login, world placement, movement, persistence, logout, removal, retry och manager ownership.

### Levererat

- [x] `BotSession`
- [x] `BotManager`
- [x] headless player classification
- [x] world placement och movement
- [x] save-enabled och no-save logout
- [x] `ManagedPlayerRemovalResult`
- [x] `RemovedPendingSave`
- [x] pending-save retry utan replay av removal
- [x] destructor safety
- [x] unit 2/2
- [x] integration 9/9
- [x] commit `e8dad3771`
- [x] pushad gren `feature/playerbots-m0-headless-player`

---

## [x] M1 — Agent Runtime, Perception och Action Contracts

### Mål

Ge botten en säker och testbar beslutsloop utan att ännu implementera full navigation eller combat.

### Leverabler

- [x] `BotObservation`
- [x] `BotPerception`
- [x] stabila creature/item/position-referenser
- [x] inga långlivade starka world pointers i observationen
- [x] `BotAction`
- [x] `BotActionResult`
- [x] normaliserade reason codes
- [x] `BotBlackboard`
- [x] prioriterad state machine eller behavior-tree-root
- [x] begränsad tick scheduler
- [x] action-rate limit
- [x] timeout, retry och backoff
- [x] strukturerad rate-limitad logging
- [x] gameplay blockeras i pending-save och closed state

### Acceptance

- [x] botten kan observera legitim närmiljö;
- [x] samma snapshot kan unit-testas deterministiskt;
- [x] stale creature-ID ger säkert `InvalidTarget`;
- [x] samma action spammas inte varje tick;
- [x] tick-budget kan mätas;
- [x] vanlig player behavior är oförändrad.

---

## [x] M2 — Lokal Navigation och World Interaction

### Mål

Navigera på ett våningsplan och hantera vanliga lokala hinder genom normala Canary-regler.

### Leverabler

- [x] M2A gångbarhetsbedömning med normaliserade värderesultat
- [x] M2A åtta riktningar och lokala positionsdelta
- [x] M2B bounded local pathfinding
- [x] M2A deterministisk diagonal kostnad enligt Canarys kostnadsskala
- [x] M2A riskkostnad för skadliga fields
- [x] M2A synliga blockerande creatures på upptagna tiles
- [x] M2B bounded dynamic repath
- [x] M2B progress-based stuck detection
- [x] stängda dörrar och use-interaction
- [x] enkel trappa/stege/teleport-transition
- [x] transitions verifieras genom observerat resultat

### M2A evidens — local walkability contract

- [x] värdebaserade observationer och resultat utan långlivade tile-, item- eller creature-referenser;
- [x] statiska terrain/item-blockers, dynamisk synlig occupancy och skadliga fields klassificeras genom verklig tile-state;
- [x] samma våning krävs och action-time revalidation körs före exakt destinationsrörelse genom `Tile::queryAdd`;
- [x] diagonal corner-semantik matchar vanlig direkt Canary-movement utan ett extra PlayerBot-hörnförbud;
- [x] dold eller oobserverad occupancy exponeras inte i planering och kan endast ge normaliserad `WorldRejected` vid servervalidering;
- [x] fokuserad gate: build 0, unit 18/18, databasbackad integration 14/14, diff 0;
- [x] source commit `8e0337c6f`;
- [x] rapportarkiv `/home/playerbots/workspace/playerbots/logs/codex-reports/20260731-201923-m2a-walkability-contract.md`;
- [x] slutreview: en blockerande floor-transition-risk fixad, upprepad review utan blocker.

### Kvarvarande issue-sized arbete

- [x] M2B — lokal pathfinding, repath och stuck detection;
- [x] M2C — dörrar och enkla world transitions.

### M2B evidens — bounded local pathfinding

- [x] värdebaserad A* över endast botens 17x13-observation, med deterministisk tie-break och explicita nod-, längd- och operationsbudgetar;
- [x] samma-våningsrutter använder M2A:s cardinal-, diagonal- och hazardkostnader och exekveras ett exakt destinationssteg i taget genom M1/M2A-validering;
- [x] tile-signaturer och statisk topologirevision invalidierar stale routes; synliga dynamiska blockerare ger begränsad repath med deterministiskt cappad backoff;
- [x] faktisk position jämförs mot expected origin/next; no-progress är evidensbaserad, begränsad och återställs efter framsteg;
- [x] terminala arrived/failed/cancelled states, Placed-lifecycle gate och teardown utan schemalagda callbacks eller world ownership;
- [x] fokuserad gate: build 0, unit 32/32, databasbackad integration 15/15, diff 0;
- [x] source commit `48db1fa1c`;
- [x] slutreview: en blockerande inadmissible heuristic fixad; upprepad review utan blocker.

### M2C evidens — doors and simple world transitions

- [x] värdebaserade interaction- och transition-kontrakt med stabil signatur, explicita terminal states, ändliga retries och cappad backoff;
- [x] vanliga dörrar använder ordinarie `Game::playerUseItem` och produktionens `Actions`/Lua-register; accepterad dispatch räknas inte som success utan observerad item- eller positionsändring;
- [x] databasbackad 1638/1639-dörr verifierar transformation, fortsatt M2B-rutt, stale rejection, level denial, dynamisk doorway-blocker och full world/databas-cleanup;
- [x] produktionens ladder-action verifierar verklig våningsändring, route invalidation, ny observation och spectator callback utan direkt Lua-callback eller positionsmutation;
- [x] ordinary-player regression använder samma action-register och verifierar oförändrad dörrdispatch;
- [x] fokuserad gate: build 0, unit 56/56, databasbackad integration 19/19, diff 0;
- [x] source commit `9a757ec71`;
- [x] rapportarkiv `/home/playerbots/workspace/playerbots/logs/codex-reports/20260731-212017-m2c-world-transitions.md`;
- [x] slutreview: terminal `ReplanRequired`-defekt och direkt teleport-fixture fixade; upprepad komplett gate utan blocker.

### Acceptance

- [x] bot navigerar från A till B runt statiska hinder;
- [x] dynamisk blockerare orsakar repath;
- [x] skadlig tile undviks när säkrare väg finns;
- [x] misslyckad movement ger backoff, inte tight loop;
- [x] bot teleporteras aldrig som vanlig unstuck-lösning;
- [x] tvåspelars spectator-fall ger korrekta callbacks.

---

## [x] M3 — Combat och Survival

### Mål

En melee-bot ska kunna välja ett tillåtet monster, positionera sig, slåss, läka och fly.

### Leverabler

- [x] M3A target discovery och combat perception
- [x] M3A deterministisk target scoring
- [x] melee range
- [x] line of sight
- [x] autoattack/follow-adapter
- [x] cooldown-aware actions
- [x] configurable healing profile
- [x] emergency healing
- [x] flee destination
- [x] reträttvägsbedömning
- [x] M3A flera observerade threats och bounded crowd-risk
- [x] M3A advisory target invalidation och release intent

### M3A evidens — combat perception och target evaluation

- [x] värdebaserade self-, creature-, policy-, candidate-, threat-, score-, selection-, lock-, intent- och failure-kontrakt utan world ownership;
- [x] observationen begränsas till M1:s legitima synfält och M2:s bounded route-evidens; synlig health-procent, observerade attacker/follow-state och botens egen damage map används utan dolda monstervärden;
- [x] default non-PvP avvisar players, NPC:er och player-owned summons samt stale, hidden, different-floor, dead, protected och unreachable targets med normaliserade skäl;
- [x] deterministisk bounded scoring, overflow-clamping, slutlig ID tie-break och hysteresis med omedelbar invalid target release;
- [x] endast advisory intents; attacked creature, follow state, movement, cooldowns, mana, health och items muteras inte;
- [x] fokuserad gate: build 0, unit 88/88, databasbackad integration 22/22, diff 0;
- [x] source commit `07458b8ba`;
- [x] slutreview: summon-master knowledge leakage och direct-distance route proxy fixades; upprepad komplett gate utan blocker.

### Kvarvarande M3-arbete

- [x] M3B — attack execution, cooldowns, range och combat positioning;
- [x] M3C — healing, survival, flee och death handling.

### M3B evidens — authoritative combat execution

- [x] M3A-valda monster re-resolves och revalideras omedelbart före mutation; players, NPC:er och summons är hårt avvisade utan PvP-opt-in;
- [x] attack assignment använder ordinarie `Game::playerSetAttackedCreature`, observerar verklig attacked-target och lämnar swing, cooldown, ammunition, events och damage till Canary;
- [x] explicit värdebaserad attack-state machine med stale/lifecycle/visibility/floor/policy/zone/LOS/range-fel, cappad backoff, ändliga retries och teardown-cancellation;
- [x] melee- och ranged-range läses från verklig utrustningsstate; M2B väljer deterministiskt bounded safe positioning och exekverar ett observerat steg i taget;
- [x] chase begränsas till route length 16, tre reposition attempts, åtta tiles från combat origin, två no-progress observations och fem sekunders timeout;
- [x] fokuserad gate: build 0, unit 123/123, databasbackad integration 26/26, diff 0;
- [x] relevant Canary monster combat/target/pathfinding regression 15/15;
- [x] source commit `702d61fc3`;
- [x] slutreview: oavsiktlig PvP-opt-in blocker fixad; upprepad komplett gate utan blocker.

### M3C evidens — survival och death handling

- [x] värdebaserade observation-, policy-, score-, healing-, flee-, state- och death-kontrakt utan kvarhållen world ownership;
- [x] deterministisk overflow-bounded urgency använder egen health/mana, recent damage, synliga hostiles, skadliga conditions, attacked-state och bounded escape-evidens;
- [x] healing re-resolves verkliga items och spells, respekterar requirements, mana och cooldown/exhaustion samt kräver observerad health-, mana- eller condition-effekt före success;
- [x] flee släpper attack/follow genom Canary, avbryter M3B och använder endast bounded M2-route med radius 8, route length 16, 128 candidates, tre attempts, två no-progress och fem sekunders timeout;
- [x] authoritative death går `DeathDetected` till terminal `Dead`, avbryter M3A/M3B/M2 och ändrar inte corpse-, loss-, save- eller ordinary-player-regler;
- [x] fokuserad gate: build 0, unit 167/167, databasbackad integration 40/40, diff 0;
- [x] ordinary network-player healing och death/corpse-regressioner är gröna;
- [x] M4-scope är explicit deferred: inga köp, restock, loot, depot, equipment, blessing, corpse recovery eller relog/respawn;
- [x] M5A:s verkliga monsterdeath-fixture bevisar normal creature-death experience attribution; M3D:s cross-subsystem contract stoppar nya transfers vid critical survival, inväntar och reconciliar en redan dispatchad ordinary move boundary, prioriterar healing/flee/death och kräver färska corpse- och inventoryobservationer före resume.
- [x] M3D fokuserad evidens: 17 deterministiska priority/ownership-tester och databas/world-fixturen `CriticalSurvivalReconcilesAuthoritativeLootBoundaryBeforeHealingAndFreshResume`; komplett gate build 0, unit 239/239, integration 52/52, diff 0.

### Acceptance

- [x] botten attackerar bara tillåtna testmonsters;
- [x] botten håller melee-avstånd;
- [x] healing respekterar mana, item count och cooldown;
- [x] healingkommando dupliceras inte före resultat;
- [x] akut health avbryter offensiv och loot;
- [x] botten kan lämna en farlig strid;
- [x] experience kommer från normal creature death.

---

## [x] M4 — Corpse Loot, Inventory och Supplies

### Mål

Hantera corpse, loot, nästlade containers, capacity och grundläggande supplies.

### Leverabler

- [x] corpse detection
- [x] corpse ownership/access
- [x] open container
- [x] nested `containerPath`
- [x] item-ID-baserade lootregler
- [x] stack count
- [x] destination backpack
- [x] capacity
- [x] full container
- [x] skydd för quest-items
- [x] okända items bevaras
- [x] supply counters
- [x] jakt avbryts vid tröskel

M4A-evidens: value-only corpse-signaturer observerar verklig monsterdöd, decay,
synlighet, M2-nåbarhet, loot-rights, top-level containerinnehåll, stack counts och
deterministiska item-ID-regler utan att öppna corpset eller flytta items. M4B
återstår för authoritative open/transfer, destination containers och full/capacity
handling; M4C återstår för supply counters och depletion decisions.

M4B-evidens: eligible corpses öppnas genom ordinary `Actions::useItem`, och
valda items skickas genom ordinary `Game::playerMoveItem`. Source/destination
deltas verifieras före success; capacity preflight, stale signatures, decay,
en pending action och teardown normaliseras med
value-only state. Nested `containerPath` och supply decisions återstår.

M4C-evidens: utrustade och burna supplies observeras som bounded value-only
item-ID-, count-, charge-, depth- och capacity-data. Konfigurerade healing-,
ammunition- och free-capacity-trösklar ger deterministiska continue, conserve,
stop och return intents. Verklig M3C-potionförbrukning ändrar nästa observation;
ordinary network-player inventory förblir oförändrat. Ekonomi, shops, depot och
equipment optimization är fortsatt deferred till M6.

M4D-evidens: deterministisk carried-container traversal använder slot/index-path,
depth 2 och container budget 16, föredrar kompatibel partial stack och därefter
första fria slot, och skiljer full, all-full, budget, capacity, incompatible och
stale destination. Real nested stack merge med begränsad requested count
reconciliar source/destination, controlled corpse removal vid authoritative
boundary rapporterar decay utan duplication, och all-full lämnar corpse och
inventory oförändrade. Komplett gate: build 0, unit 256/256, integration 55/55,
diff 0. M3D survival interruption ingår i den fulla M4-auditen.

### Acceptance

- [x] corpse öppnas genom normal serveraction;
- [x] konfigurerat loot flyttas till rätt container;
- [x] otillräcklig capacity hanteras utan loop;
- [x] protected och unknown items säljs eller kastas inte;
- [x] supply- och capacity-trösklar kan stoppa jakten.

---

## [x] M5 — Autonomous Adventure Loop och Level Progression

### Mål

Knyta ihop perception, navigation, combat, survival och loot till en autonom jaktcykel.

### Loop

`Safe point → huntingområde → combat → loot → resurskontroll → fortsätt/retur → logout`

### Leverabler

- [x] hunting-area definition
- [x] allowed monster profile
- [x] säker start- och returposition
- [x] hunt goal
- [x] kill/XP counters
- [x] supply/capacity exit conditions
- [x] death detection
- [x] temple recovery
- [x] resume efter recovery
- [x] save/logout genom M0

M5A-evidens: en session-owned, value-only coordinator sekvenserar bounded lokal
M2-travel, M3 target/attack, M3C survival/death precedence, M4 corpse/loot och
M4C supply/return intents. En kompakt databas/world-fixture går från startregion
till huntregion, tilldelar ett verkligt monster som target, går genom normal
creature death med legitim XP, lootar ett verkligt item, observerar supplies,
repathar runt en dynamisk blocker och återvänder. Separat verklig player death
gör coordinatorn terminal och session close lämnar ingen retained ownership.
M5B-evidens utökar policyn med deterministiskt ordnade, explicit konfigurerade
regioner och ändliga kill-, combat-, recovery-, route- och durationgränser. En
databas/world-kampanj utför tio separata M3-target/combat och normal
creature-death-cykler, observerar auktoritativ XP, lootar varje corpse via M4,
och omvärderar supplies mellan cykler. Safe-boundary logout sparar XP och
inventory via ordinarie M0/IOLoginData-livscykel; en ny Player och BotSession
laddar samma progression utan transient target, route eller adventure-state.
En separat auktoritativ player-death kör normal corpse/loss/save-livscykel,
persisterar temple-position och återinloggning rekonstruerar en value-only
kampanj först efter en färsk observation i den konfigurerade recovery-regionen.
Det fokuserade gate-resultatet omfattar minst 274 unit- och 57
databas-backed integrationstester. Produktionssoak och dynamisk global
hunting-area-optimering är uttryckligen senare validering, inte M5-acceptans.

### Acceptance

- [x] bot dödar minst 10 testmonsters autonomt;
- [x] bot får legitim experience;
- [x] bot lootar minst ett konfigurerat item;
- [x] bot återvänder vid resursgräns;
- [x] bot överlever eller hanterar minst en death/recovery-cykel;
- [x] logout lämnar ingen world placement;
- [x] ny login visar persisterad progression.

---

## [ ] M6 — Economy, Equipment och Resupply

### Mål

Botten ska kunna återställa sin jaktberedskap och förbättra sig genom normal ekonomi.

### Leverabler

- [x] item valuation/profile
- [ ] sellable loot
- [ ] buy list
- [ ] NPC shop adapter
- [ ] mat, potions och ammunition
- [x] equipment comparison
- [ ] equip/unequip
- [ ] pengar och köpbudget
- [ ] depot eller definierad storage
- [ ] återuppta avbruten hunt

M6A-evidens: värdebaserade observationer av egen utrustning och bounded burna
containers använder intrinsic player-visible itemmetadata, explicit PlayerBot-policy
och legitimt kända NPC-priser som separata källor. Deterministisk rollspecifik
scoring täcker requirements, slots, weapon/armor/shield-stats, range, modifiers,
duration, weight, supplies, hysteresis, overflow och stabila tie-breakers utan att
flytta, utrusta, köpa, sälja eller behålla Item/Container-ägarskap. Komplett gate:
build 0, unit 300/300, databasbackad integration 62/62, diff 0. Source commit
`6df23c044`. M6B shoptransaktioner och M6C depot/resupply/equipment execution
återstår; full M6 är fortsatt öppen.

### Acceptance

- [ ] bot säljer endast tillåtna loot-items;
- [ ] bot köper supplies utan direkt money/item mutation;
- [ ] bot utrustar ett verifierbart bättre item;
- [ ] otillräckliga pengar leder till ny plan;
- [ ] resupply följs av återgång till jakt.

---

## [ ] M7 — NPC och Quest Engine

### Mål

Skapa ett generellt, serverauktoritativt questflöde.

### Objective-typer

- [ ] prata med NPC
- [ ] besök plats
- [ ] döda creatures
- [ ] samla items
- [ ] använd item/world object
- [ ] leverera items
- [ ] besegra specifikt target
- [ ] sekventiella objectives
- [ ] prerequisites
- [ ] reward collection

### Leverabler

- [ ] `QuestDefinition`
- [ ] `QuestProgress`
- [ ] `BotQuestPlan`
- [ ] structured NPC adapter där möjligt
- [ ] metadata/adapters för Lua-baserade quests
- [ ] storage används som serverauktoritativ signal
- [ ] botten skriver aldrig quest-storage direkt
- [ ] retry och idempotens
- [ ] restart/resume

### Acceptance

- [ ] representativ flerstegsquest slutförs autonomt;
- [ ] kedjan innehåller NPC, kill, collect, exploration/use och turn-in;
- [ ] reward skapas av normalt questflöde;
- [ ] restart mitt i questen tappar eller duplicerar inte progress;
- [ ] fel NPC-svar eller saknat item ger säker omplanering.

---

## [ ] M8 — Long-Horizon Progression Planner

### Mål

Välja långsiktiga mål baserat på vocation, level, skills, ekonomi, equipment, access och quests.

### Leverabler

- [ ] progression goals
- [ ] goal prerequisites
- [ ] hunting-area suitability
- [ ] equipment target
- [ ] quest/access target
- [ ] skill target
- [ ] economy target
- [ ] risk- och failure history
- [ ] persistent planner state
- [ ] resume efter restart
- [ ] deterministisk baselineplanner

### Acceptance

- [ ] låg-level-bot når konfigurerat mål-level;
- [ ] bot byter huntingområde när det gamla blir olämpligt;
- [ ] bot kan prioritera en access quest före nästa jaktområde;
- [ ] misslyckade mål blacklistas tillfälligt;
- [ ] persistent plan kan migreras mellan planner-versioner.

---

## [ ] M9 — Socialt Beteende, Fleet och Production Hardening

### Mål

Köra flera säkra bots tillsammans med vanliga spelare under längre tid.

### Leverabler

- [ ] party join/leave
- [ ] follow leader
- [ ] enkla party-roller
- [ ] target- och loot-respekt
- [ ] fleet start/stop
- [ ] concurrency limits
- [ ] global tick budget
- [ ] metrics
- [ ] watchdog
- [ ] graceful shutdown
- [ ] server reload/restart
- [ ] per-bot debug logging
- [ ] soak harness
- [ ] admin controls
- [ ] allowlist för botkonton

### Acceptance

- [ ] flera bots spelar samtidigt utan lifecycle-läckor;
- [ ] vanliga players får inga blockerande regressioner;
- [ ] lång soak visar stabil session-, memory- och tick-latency;
- [ ] shutdown lämnar inga online eller world-placerade bots;
- [ ] full relevant CTest-svit passerar;
- [ ] sanitizer/fault-injection-resultat är dokumenterade;
- [ ] slutreview hittar ingen blocker.

---

# V1 Release Gate

- [ ] M0–M9 är markerade klara med evidens.
- [ ] End-to-end-demonstrationen i visionen är reproducerbar.
- [ ] Full relevant testsvit passerar.
- [ ] Multi-bot soak passerar.
- [ ] Vanliga player-flöden har regressionstäckning.
- [ ] Dokumentation för drift, felsökning och konfiguration finns.
- [ ] Slutlig read-only review hittar inga blockerande fel.
