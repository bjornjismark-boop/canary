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

## [ ] M1 — Agent Runtime, Perception och Action Contracts

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

## [ ] M2 — Lokal Navigation och World Interaction

### Mål

Navigera på ett våningsplan och hantera vanliga lokala hinder genom normala Canary-regler.

### Leverabler

- [ ] gångbarhetsbedömning
- [ ] åtta riktningar
- [ ] lokal pathfinding
- [ ] diagonal kostnad
- [ ] riskkostnad för fields och threats
- [ ] upptagna tiles
- [ ] repath
- [ ] stuck detection
- [ ] stängda dörrar och use-interaction
- [ ] enkel trappa/stege/teleport-transition
- [ ] transitions verifieras genom observerat resultat

### Acceptance

- [ ] bot navigerar från A till B runt statiska hinder;
- [ ] dynamisk blockerare orsakar repath;
- [ ] skadlig tile undviks när säkrare väg finns;
- [ ] misslyckad movement ger backoff, inte tight loop;
- [ ] bot teleporteras aldrig som vanlig unstuck-lösning;
- [ ] tvåspelars spectator-fall ger korrekta callbacks.

---

## [ ] M3 — Combat och Survival

### Mål

En melee-bot ska kunna välja ett tillåtet monster, positionera sig, slåss, läka och fly.

### Leverabler

- [ ] target discovery
- [ ] target scoring
- [ ] melee range
- [ ] line of sight
- [ ] autoattack/follow-adapter
- [ ] cooldown-aware actions
- [ ] configurable healing profile
- [ ] emergency healing
- [ ] flee destination
- [ ] reträttvägsbedömning
- [ ] flera threats
- [ ] stop attack och target invalidation

### Acceptance

- [ ] botten attackerar bara tillåtna testmonsters;
- [ ] botten håller melee-avstånd;
- [ ] healing respekterar mana, item count och cooldown;
- [ ] healingkommando dupliceras inte före resultat;
- [ ] akut health avbryter offensiv och loot;
- [ ] botten kan lämna en farlig strid;
- [ ] experience kommer från normal creature death.

---

## [ ] M4 — Corpse Loot, Inventory och Supplies

### Mål

Hantera corpse, loot, nästlade containers, capacity och grundläggande supplies.

### Leverabler

- [ ] corpse detection
- [ ] corpse ownership/access
- [ ] open container
- [ ] nested `containerPath`
- [ ] item-ID-baserade lootregler
- [ ] stack count
- [ ] destination backpack
- [ ] capacity
- [ ] full container
- [ ] skydd för quest-items
- [ ] okända items bevaras
- [ ] supply counters
- [ ] jakt avbryts vid tröskel

### Acceptance

- [ ] corpse öppnas genom normal serveraction;
- [ ] konfigurerat loot flyttas till rätt container;
- [ ] otillräcklig capacity hanteras utan loop;
- [ ] protected och unknown items säljs eller kastas inte;
- [ ] supply- och capacity-trösklar kan stoppa jakten.

---

## [ ] M5 — Autonomous Adventure Loop och Level Progression

### Mål

Knyta ihop perception, navigation, combat, survival och loot till en autonom jaktcykel.

### Loop

`Safe point → huntingområde → combat → loot → resurskontroll → fortsätt/retur → logout`

### Leverabler

- [ ] hunting-area definition
- [ ] allowed monster profile
- [ ] säker start- och returposition
- [ ] hunt goal
- [ ] kill/XP counters
- [ ] supply/capacity exit conditions
- [ ] death detection
- [ ] temple recovery
- [ ] resume efter recovery
- [ ] save/logout genom M0

### Acceptance

- [ ] bot dödar minst 10 testmonsters autonomt;
- [ ] bot får legitim experience;
- [ ] bot lootar minst ett konfigurerat item;
- [ ] bot återvänder vid resursgräns;
- [ ] bot överlever eller hanterar minst en death/recovery-cykel;
- [ ] logout lämnar ingen world placement;
- [ ] ny login visar persisterad progression.

---

## [ ] M6 — Economy, Equipment och Resupply

### Mål

Botten ska kunna återställa sin jaktberedskap och förbättra sig genom normal ekonomi.

### Leverabler

- [ ] item valuation/profile
- [ ] sellable loot
- [ ] buy list
- [ ] NPC shop adapter
- [ ] mat, potions och ammunition
- [ ] equipment comparison
- [ ] equip/unequip
- [ ] pengar och köpbudget
- [ ] depot eller definierad storage
- [ ] återuppta avbruten hunt

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
