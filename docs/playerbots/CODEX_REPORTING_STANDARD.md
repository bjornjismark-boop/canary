# Codex Reporting Standard för PlayerBots

## Syfte

Codex-terminalens transcript får aldrig vara den enda källan till vad som gjordes.

Varje större Codex-uppgift ska avslutas med en komplett, läsbar Markdown-rapport som kan öppnas, laddas upp eller återges från en tmux-session där terminalutdata är trunkerad.

## Kanoniska sökvägar

Rapportkatalog:

```bash
$PLAYERBOTS_ROOT/logs/codex-reports
```

Rullande senaste rapport:

```bash
$PLAYERBOTS_ROOT/logs/codex-reports/playerbots-current.md
```

Arkiverad rapport:

```bash
$PLAYERBOTS_ROOT/logs/codex-reports/YYYYMMDD-HHMMSS-<branch>-<task>.md
```

Aktuell patch när uppgiften ändrar kod:

```bash
$PLAYERBOTS_ROOT/logs/codex-reports/playerbots-current.patch
```

Rapportfilerna ligger utanför Canary-repot och ska normalt inte stageas eller committas.

## Obligatorisk arbetsregel

Vid slutet av varje uppgift ska Codex:

1. skriva hela rapporten till en temporär fil;
2. verifiera att filen är icke-tom;
3. atomiskt ersätta `playerbots-current.md`;
4. kopiera samma innehåll till en tidsstämplad arkivfil;
5. skapa eller uppdatera patchen om kod ändrats och användaren begärt patch;
6. beräkna storlek och SHA-256;
7. skriva ut en kompakt handoff som ryms i terminalen.

Codex får inte hävda att rapporten är uppdaterad utan att visa ny `stat` och SHA-256.

## Rapportformat

Varje rapport ska börja med en kompakt handoff som ryms på ungefär 25 rader.

```markdown
# PlayerBots Codex Handoff

Status: PASS | PARTIAL | BLOCKED
Task: <kort namn>
Branch: <branch>
Base commit: <sha>
Head commit: <sha eller WORKTREE>
Source changed: yes | no
Tests: <kort sammanfattning>
Review: <kort sammanfattning>
Blocking findings: <antal>
Report generated: <ISO-8601>
```

Därefter ska rapporten innehålla följande avsnitt.

## 1. Uppgift och avgränsning

- exakt mål;
- vad som uttryckligen inte ingick;
- användarens viktiga krav;
- om uppgiften var read-only eller fick ändra kod.

## 2. Utgångsläge

- repo;
- branch;
- HEAD;
- working-tree-status före arbetet;
- relevanta tidigare milstolpar och commits.

## 3. Analys och beslut

- root cause eller arkitekturproblem;
- call paths och lifecycle;
- designalternativ;
- valt alternativ och varför;
- förkastade alternativ och varför.

Påståenden ska ha exakta fil- och linjereferenser.

## 4. Ändringar

För varje ändrad fil:

- sökväg;
- vad som ändrades;
- varför;
- observerbar beteendeförändring;
- risk för ordinary-player-regression.

Untracked filer ska uttryckligen listas.

## 5. State machines och invariants

När uppgiften påverkar lifecycle eller AI-state:

- alla states;
- lagliga transitions;
- failure transitions;
- retry behavior;
- ownership;
- terminal states;
- invariants före och efter action.

## 6. Tester och verifiering

För varje kommando:

```text
Command:
<exakt kommando>

Exit code:
<värde>

Result:
<pass/fail/blockerad och relevant output>
```

Minst följande ska redovisas när relevant:

- build;
- unit tests;
- integration tests;
- `git diff --check`;
- full CTest eller uttrycklig uppgift att den inte kördes;
- sanitizer, Valgrind, fault injection eller uttrycklig uppgift att de inte kördes;
- Windows-build eller uttrycklig uppgift att den inte kördes.

Codex får aldrig ersätta ett blockerat test med ett påstående om att testet passerade någon annanstans, om resultatet inte har tillhandahållits och dokumenterats.

## 7. Review findings

Fynd ordnas efter:

1. blockerande defekter;
2. definite non-blocking defects;
3. risker;
4. optional improvements;
5. validation gaps.

Varje fynd ska ha:

- severity;
- fil och linje;
- scenario;
- konsekvens;
- rekommenderad åtgärd.

Om inga blockerare finns ska rapporten uttryckligen säga det.

## 8. Milestone status

Rapporten ska ange:

- aktuell milstolpe;
- vilka acceptance-kriterier som bevisats;
- vilka som återstår;
- vilken evidens som stödjer varje avprickning;
- om roadmapen bör uppdateras.

Codex får inte markera en milestone färdig enbart för att implementationen finns.

## 9. Git och artefakter

- `git diff --check`;
- `git status --short --branch`;
- staged/unstaged/untracked;
- commit eller avsaknad av commit;
- push eller avsaknad av push;
- rapportens path, storlek och SHA-256;
- patchens path, storlek och SHA-256;
- patch apply-check-resultat om patch skapats.

## 10. Kvarvarande arbete

- blockerare;
- öppna risker;
- saknade tester;
- rekommenderad nästa issue-sized uppgift;
- vad nästa Codex-session måste läsa först.

## Sanningsregler

Codex ska alltid:

- skilja på byggt, testat, granskat, committat och pushat;
- skilja på sandbox-resultat och externa resultat;
- använda exakta testantal;
- redovisa blockerade tester;
- säga när full CTest inte körts;
- säga när en fil inte kunnat skrivas;
- undvika påståenden som inte stöds av kommandoutdata.

## Kompakt terminalhandoff

Efter rapportskrivning ska Codex endast behöva skriva ut:

```text
REPORT_STATUS=PASS|PARTIAL|BLOCKED
REPORT_PATH=<path>
REPORT_ARCHIVE=<path>
REPORT_SIZE=<bytes>
REPORT_SHA256=<sha256>
PATCH_PATH=<path or NONE>
PATCH_SHA256=<sha256 or NONE>
BUILD=<result>
UNIT=<result>
INTEGRATION=<result>
DIFF_CHECK=<result>
BLOCKERS=<count>
NEXT=<kort nästa steg>
```

Samt:

```bash
git status --short --branch
```

## Kommandon för användaren

Visa handoff:

```bash
sed -n '1,40p' \
  "$PLAYERBOTS_ROOT/logs/codex-reports/playerbots-current.md"
```

Visa full rapport utan pager:

```bash
cat \
  "$PLAYERBOTS_ROOT/logs/codex-reports/playerbots-current.md"
```

Verifiera rapport:

```bash
stat -c '%y  %s bytes  %n' \
  "$PLAYERBOTS_ROOT/logs/codex-reports/playerbots-current.md"

sha256sum \
  "$PLAYERBOTS_ROOT/logs/codex-reports/playerbots-current.md"
```

Kopiera rapporten till en lättuppladdad plats:

```bash
cp \
  "$PLAYERBOTS_ROOT/logs/codex-reports/playerbots-current.md" \
  "$HOME/playerbots-current.md"
```

## Standardinstruktion att lägga sist i varje Codex-uppgift

```text
Before finishing, write a complete self-contained Markdown report following
docs/playerbots/CODEX_REPORTING_STANDARD.md.

Write it atomically to:

$PLAYERBOTS_ROOT/logs/codex-reports/playerbots-current.md

Also copy it to a timestamped archive file in the same directory.

Do not rely on terminal output as the only report.

Print only the compact terminal handoff defined by the reporting standard,
followed by git status --short --branch.
```
