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
