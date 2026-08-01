# PlayerBots live-soak operator guide

The live harness is prerequisite infrastructure. A smoke result is not M9E
release evidence and always records `PRODUCTION_SOAK=NO`.

## Safety contract

Use a dedicated host and an explicitly disposable database. The harness refuses
non-loopback `config.lua`, ambiguous database identities, a missing executable,
unbounded duration/population, and release profiles with fewer than three
restarts. It records and signals only the child PID it launched, and verifies
the Linux `/proc` start-time identity before every RSS sample and signal.

The database environment file must be mode 0600 and provide:

```text
TEST_DB_NAME=playerbots_soak_unique
TEST_DB_ALLOW_RESET=1
```

Use the existing `TEST_DB_HOST`, `TEST_DB_USER`, `TEST_DB_PASSWORD`,
`TEST_DB_PORT`, and `TEST_DB_SCHEMA` keys. The harness resets the explicitly
authorized disposable database, loads the repository schema and pre-launch
fixtures, and drops it after shutdown.
Credentials are passed through private temporary MySQL and ordinary-client environment
files, not command arguments. The repository client authenticates, enters the world, remains connected,
sends harmless protocol heartbeats, reconnects after a server restart, and exits
nonzero on an unexpected disconnect. It must never use BotManager.

Canary has no `--config` option. The harness creates a private isolated runtime
directory, copies the explicit template to `config.lua`, appends loopback ports
and disposable database values, generates `config/playerbots.json`, and links
only required repository data directories. It never edits the source config.

The harness automatically launches `tools/playerbots_ordinary_client.py`; operators
do not construct `SOAK_CLIENT_COMMAND` or pass client credentials on the command line.

`PLAYERBOTS_SOAK_OUTPUT` enables the server-side local adapter. The harness sets
it to the private run directory. The adapter has no listener, is disabled by
default, copies M9D snapshots, consumes bounded JSONL requests through M9C, and
exports an independent bounded dispatcher histogram. Never set it manually for
a production server.

## Smoke

```sh
tools/run-playerbots-live-soak.sh \
  --env /home/playerbots/.config/playerbots/canary-test.env \
  --config /home/playerbots/workspace/playerbots/canary/config.lua.dist \
  --server /home/playerbots/workspace/playerbots/canary/canary \
  --profile smoke \
  --duration-seconds 90 \
  --bots 4 \
  --output-root /home/playerbots/workspace/playerbots/logs/playerbots-soak
```

The verified Linux release build uses the repository preset and the standalone
`canary` target:

```sh
cmake --preset linux-release
cmake --build --preset linux-release --target canary -j2
```

The resulting standalone server is `/home/playerbots/workspace/playerbots/canary/canary`.

## Latest live smoke evidence

The 2026-08-01 mixed live smoke ran for 90 seconds with four managed PlayerBots,
one ordinary protocol client, and one controlled restart. It passed with zero
invariant failures, 51,047 dispatcher samples, and 92 RSS data samples. The
server and client logged out cleanly, both server shutdowns returned zero, and
the disposable database was dropped. Artifacts are under
`/home/playerbots/workspace/playerbots/logs/playerbots-soak/20260801-185727-1680658`.
The server executable SHA-256 is
`ac92a4c8b2420c6e872466c260ef26ed63e3d7654f5cc92a12f02db34061b746`;
the artifact manifest SHA-256 is
`682570bdb666ca394558d1b6474ffe0e919b5eeccd30172a06d3849fe69f7463`.
This is smoke evidence only: `PRODUCTION_SOAK=NO` and `RELEASE_SOAK=NOT_RUN`.

## Ping-timeout regression evidence

The 2026-08-01 pre-release regression smoke ran for 600 seconds with 20 managed
PlayerBots, one ordinary protocol player, and one completed controlled restart.
It passed with peak population 20/20, zero managed ping timeouts, zero unexpected
managed logouts or logins, zero unscheduled session replacements, 463,888
dispatcher samples, 760 clean invariant evaluations, and cleanup PASS. RSS was
1,420,728 KiB initially, 1,423,084 KiB peak, and 1,422,560 KiB final. Artifacts
are under `/home/playerbots/workspace/playerbots/logs/playerbots-soak/20260801-201026-1708785`.
The standalone executable SHA-256 is
`32873a7813b9320799e70bd8a0f0e8433fb4327016ee2e311a31c0bc9e4932c4`;
the artifact manifest SHA-256 is
`d8ac8a292e664f34e102e31a710b96a6d976d890553214770f2d3347cf91cfa4`.
This remains pre-release smoke evidence only: `PRODUCTION_SOAK=NO`,
`RELEASE_SOAK=NOT_RUN`, `M9E_COMPLETE=NO`, and `M9_COMPLETE=NO`.

## Preserved failed diagnostic

The interrupted 2026-08-01 release-profile diagnostic is preserved unchanged at
`/home/playerbots/workspace/playerbots/logs/playerbots-soak/20260801-191251-1686585`.
It requested 7,200 seconds, 20 managed bots, one ordinary player, and three
restarts, but was interrupted after 12 distinct managed bots were removed by
the network ping timeout. Reconciliation hid that churn behind a final
20-managed/20-placed snapshot. The result is FAIL, cleanup is PASS,
`releaseSoak` is `NOT_RUN`, and its zero-byte `invariants.json` records the
harness defect fixed by the subsequent churn-aware invariant writer. The
preserved `SHA256SUMS` file has SHA-256
`886dc71b49f3f01fdbc9727addde4fd9b2971ea85d9ac9fecf6b9153afbb0935`.

## Final release soak

The qualifying release soak has not run. The exact approved rerun command is:

```sh
tools/run-playerbots-live-soak.sh \
  --env /home/playerbots/.config/playerbots/canary-test.env \
  --config /home/playerbots/workspace/playerbots/canary/config.lua.dist \
  --server /home/playerbots/workspace/playerbots/canary/canary \
  --profile release \
  --duration-seconds 7200 \
  --bots 20 \
  --restarts 3 \
  --output-root /home/playerbots/workspace/playerbots/logs/playerbots-soak
```

Do not set `PRODUCTION_SOAK=YES`, `M9E_COMPLETE=YES`, or `M9_COMPLETE=YES`
unless this qualifying run genuinely completes with its release criteria met.
