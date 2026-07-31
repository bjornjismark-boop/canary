# PlayerBots — North Star

**Status:** Approved direction
**M0 foundation:** `e8dad3771` — `feat: add headless player bot lifecycle`
**Scope:** Canary server-side headless players
**Last updated:** 2026-07-31

## Vision

PlayerBots ska bli serverstyrda, autonoma spelare som kan leva ett helt spelarliv i Canary genom samma regler som vanliga spelare.

En PlayerBot ska kunna börja som en vanlig låg-level-karaktär, observera sin omgivning, planera säkra handlingar, resa i världen, bekämpa monsters, läka, fly, loota, hantera inventory och supplies, förbättra sin utrustning, utveckla level och skills, interagera med NPC:er, genomföra quests, dö, återhämta sig och fortsätta efter logout eller serveromstart.

Målet är inte att simulera en nätverksklient eller att skapa en bot som fuskar genom intern serverkunskap. Målet är att skapa en **server-native AI-spelare** som använder normala gameplay-regler och som är säker, observerbar, testbar och skalbar.

## Slutlig demonstrationsberättelse

En ny testkaraktär startas som PlayerBot utan nätverksklient.

Botten:

1. loggar in genom den etablerade M0-livscykeln;
2. köper eller hämtar grundläggande supplies;
3. navigerar från stad till ett lämpligt huntingområde;
4. väljer targets utifrån risk, vocation och mål;
5. slåss, positionerar sig, läker och flyr vid behov;
6. lootar corpses och hanterar nästlade containers;
7. återvänder när supplies, ammunition eller capacity kräver det;
8. säljer loot, köper nya supplies och förbättrar utrustning;
9. levlar och utvecklar relevanta skills genom legitim gameplay;
10. genomför en representativ flerstegsquest med NPC-dialog, kill-, collect-, exploration- och turn-in-moment;
11. hanterar död och återupptar sin plan;
12. klarar logout, serveromstart och återinloggning utan förlorad eller duplicerad progression;
13. kan spela tillsammans med andra bots och vanliga spelare utan att bryta deras gameplay;
14. kan köras under längre tid utan läckta sessioner, fastnade actionloopar eller databaskorruption.

## Produktprinciper

### 1. Servern är auktoritativ

PlayerBots får inte:

- skriva level, experience, skills, pengar eller quest-state direkt;
- skapa eller duplicera items;
- teleportera sig som normal navigation;
- kringgå cooldowns, capacity, line of sight, accesskrav eller combatregler;
- hoppa över NPC-, item-, action- eller questvalidering.

All progression ska uppstå genom samma serverauktoritativa gameplay som för vanliga spelare.

### 2. Begränsad observation

Att botten kör i servern innebär inte att den får använda all serverkunskap.

Botten ska normalt bara känna till:

- den egna spelarens tillåtna state;
- synliga tiles, creatures och items;
- inventory och containers som spelaren själv äger eller har öppnat;
- kända mål och tidigare observerade transitions;
- serverauktoritativ quest-progress som spelaren legitimt har tillgång till.

Dolda spelare, osynliga monsters, framtida spawninformation, privata inventoryn och annan otillgänglig state får inte användas i beslutsfattandet.

### 3. Deterministisk kärna först

Navigation, combat, healing, loot, inventory, NPC-interaktion, quest objectives och recovery ska fungera utan extern LLM.

En framtida språkmodell får hjälpa till med:

- långsiktig planering;
- personlighet;
- social dialog;
- val mellan redan validerade planer.

Den får inte direkt styra varje steg, attack eller item-action.

### 4. Lifecycle och gameplay hålls separerade

M0-komponenterna fortsätter ansvara för:

- sessionägarskap;
- login och logout;
- world removal;
- persistence;
- pending-save;
- retry;
- cleanup.

Gameplay-AI:n ska ligga i separata komponenter och får inte göra `BotSession` eller `BotManager` till AI-monoliter.

### 5. Varje handling måste kunna förklaras

Varje viktigt beslut ska kunna loggas med:

- botens namn och GUID;
- observation eller relevant state;
- valt goal;
- valt action;
- reason code;
- action-resultat;
- retry count;
- tick duration.

Det ska gå att förstå varför en bot agerade utan att läsa en privat modellresonemangskedja.

### 6. Begränsad belastning

Varje bot ska ha:

- konfigurerbar tickfrekvens;
- action-rate limits;
- CPU-budget;
- retry-gränser;
- timeout;
- exponentiell backoff;
- säker idle-state.

Hundratals bots får inte skapa obegränsade loops eller dominera serverns scheduler.

### 7. Vanliga spelare får inte påverkas negativt

Nya PlayerBot-abstraktioner måste bevara:

- vanlig player login/logout;
- ordinary `Game::removeCreature`;
- protocol-facing behavior;
- spectator callbacks;
- party, guild, trade, channel och summon-semantik;
- befintliga CMake- och Visual Studio-byggen.

## Övergripande kapabiliteter

### Perception och actions

- stabil observation snapshot utan långlivade world pointers;
- synliga tiles och creatures;
- resurser, cooldowns och conditions;
- inventory och nästlade containers;
- strukturerade actions och action-resultat;
- event- eller cooldownbaserad väntan.

### Navigation

- åtta riktningar;
- blockerade och upptagna tiles;
- farliga fields;
- lokal A* eller befintlig Canary-pathfinding;
- repath och stuck detection;
- dörrar, trappor, hål, rope spots, teleports och transport-NPC:er;
- flera våningsplan och områdesgraf.

### Combat och survival

- target scoring;
- melee- och distance-positionering;
- line of sight och attack range;
- autoattack, spells, runes och usable items;
- healingprofiler;
- emergency flee;
- reträttvägar;
- flera samtidiga threats.

### Inventory, loot och ekonomi

- container paths;
- item-ID-baserad klassificering;
- equip/unequip;
- corpse opening;
- lootregler;
- capacity och container slots;
- supplies;
- buy/sell;
- skydd för quest-items och okända items.

### Progression och quests

- legitim experience och skill progression;
- NPC-interaktion;
- strukturerade quest objectives;
- storage och serverevents som auktoritativ progress;
- kill, collect, visit, use, deliver och boss objectives;
- prerequisites, rewards och access quests;
- långsiktig progression planner.

### Socialt och fleet management

- party och follow;
- grundläggande roller;
- respekt för andra spelares targets och lootregler;
- flera samtidiga bots;
- start/stop-scheman;
- metrics, watchdog och soak testing;
- graceful shutdown och server restart.

## V1 Definition of Done

PlayerBots V1 är klart när en reproducerbar miljö visar att:

- en låg-level-bot kan levla genom legitim gameplay;
- botten kan genomföra en representativ flerstegsquest;
- navigation, combat, healing, loot, inventory och resupply fungerar tillsammans;
- botten kan dö och fortsätta;
- serveromstart mitt i progressionen inte duplicerar eller tappar state;
- vanliga spelare inte får funktionella regressioner;
- flera bots klarar ett längre soak-test;
- relevanta unit-, integration- och full-suite-tester passerar;
- slutlig read-only review inte hittar blockerande fel.

## Icke-mål för V1

V1 kräver inte:

- stöd för varje quest på hela servern;
- mänskligt perfekt PvP;
- fri mänsklig konversation;
- imitation av en specifik riktig spelare;
- LLM som gameplay-krav;
- automatisk förståelse av godtyckliga Lua-skript;
- perfekt navigation i varje specialbyggd map feature;
- användning mot externa eller officiella servrar.
