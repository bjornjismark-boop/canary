# PlayerBot fleet operations

The fleet controller, configuration, telemetry, and administration boundaries are server-side Canary components. They do not expose a web UI or MyAAC integration.

## Telemetry

`BotFleetTelemetrySnapshot` is an immutable value snapshot. It contains aggregate lifecycle counts, planner/session health, coordination counts, command pressure, and fixed normalized failure buckets. It never contains player names, character IDs, credentials, `Player` objects, or `BotSession` ownership. Prometheus-enabled builds export only fixed metric names and bounded enum reason labels; per-member labels are prohibited.

Events and audit entries are bounded newest-first histories. Treat them as operational diagnostics, not a durable audit database.

## Administrative service boundary

The transport policy defaults to `Disabled`; Canary does not open another listener. An embedding transport must explicitly select `Localhost` or `UnixSocket`, keep its listener local, provide an authentication callback, and pass the decoded request size. Remote/public binding is outside this contract.

Authenticated reads support status, bounded audit entries, and bounded telemetry events. Authenticated writes only enqueue M9C commands. An accepted response means queued, not completed; completion is observed later through command audit/status. Requests are subject to global fixed-window rate limits and request/response size limits. Credentials are transient and are never copied into snapshots, events, metrics, or audit details.

## Shutdown

Shutdown stops the admin service before telemetry, configuration administration, fleet reconciliation, and BotManager teardown. After stop, requests fail safely and no authentication callback is retained.

# Release-hardening and soak profiles

PlayerBots remain disabled by default. Keep the smoke profile (two managed bots)
for initial validation and increase population only after the preceding profile
passes. The built-in deterministic profiles are `smoke`, `small`, `medium`,
`large`, and `release-candidate`; none selects a production database or enables
an administration transport.

The resource policy has non-overridable ceilings for managed population,
pending login/logout work, command queue depth, planner actions, and
reconciliation work. Dispatcher or scheduler pressure first stops new bot
logins and optional planner expansion, then reduces reconciliation and optional
telemetry/coordination work. Save, logout, shutdown, and ordinary-player work
remain protected. Recovery uses consecutive healthy observations rather than an
immediate oscillating resume.

Run the deterministic release checks with:

```sh
UNIT_FILTER='PlayerBotFleetHardeningTest.*' \
INTEGRATION_FILTER='PlayerBotIntegrationTest.FleetHardening*' \
  /home/playerbots/workspace/playerbots/tools/run-playerbots-gate.sh
```

Build and run the standalone deterministic soak tool with an explicit disposable
database environment and durable output path:

```sh
cmake --build build/linux-debug --target playerbots_soak
build/linux-debug/tests/playerbots_soak \
  --profile medium \
  --environment /path/to/canary-test.env \
  --output /path/to/playerbots-soak-report.txt
```

The tool refuses environments unless `TEST_DB_ALLOW_RESET=1` and the database
name is visibly test-specific. `--ticks` may override the selected profile only
within the absolute 10,000,000-tick ceiling. Output is written through a
temporary file and renamed only after the terminal result is complete. The
report labels deterministic execution and `PRODUCTION_SOAK=NO`; it must not be
presented as a live-server soak.

This command is bounded and uses the disposable test database configured by the
gate. It is not a production soak. A production candidate must separately run a
time-bounded mixed human/bot soak against an explicitly disposable environment,
record population, wall-clock duration, tick latency, process RSS, cleanup, and
restart cycles, and retain its terminal report. Never infer CPU or memory values
from unsupported counters.

For overload, pause new work first, use `drain` to remove low-priority bots at
safe save boundaries, and leave ordinary players connected. On database failure,
do not report logout complete until save succeeds. On restart, use gradual login
and verify no duplicate session, stale coordination reservation, or invalid
planner checkpoint. Shut down the admin service before telemetry, command,
fleet, and manager ownership.

Admin transport remains disabled or local-only and authenticated. Provision
credentials outside Git, place TLS at a trusted reverse proxy, retain only
bounded redacted audit/event history, and never expose secrets in query strings
or logs. Roll back by disabling the fleet and draining it safely; do not delete
sessions or gameplay state through SQL. The GUI is a separate thin
administration layer and never owns gameplay state.
