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
After authoritative placement, the client keeps independent monotonic schedules for
the legacy `0x1E` pong and a harmless direction turn every 300 seconds. The activity
alternates `0x6F` (north) and `0x70` (east); Canary's normal `Game::playerTurn` path
resets ordinary-player idle time without moving the character. Override
`SOAK_ACTIVITY_SECONDS` only with a finite value from 1 through 600 seconds.

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

The later 2026-08-02 release diagnostic is preserved unchanged at
`/home/playerbots/workspace/playerbots/logs/playerbots-soak/20260802-072937-1910561`.
Its ordinary client remained network-live through pongs but was removed after
approximately 963.927 seconds by the normal 15-minute idle-player enforcement.
It is failed diagnostic evidence only: result FAIL, client exit 1, cleanup PASS,
and `releaseSoak` `NOT_RUN`. Its manifest SHA-256 is
`f480a2fccb2ca43b44075e5343b7962e1565951a6ab1ec4c18bde63c585d9c35`.

## Ordinary idle regression evidence

The 2026-08-02 pre-release regression ran for 1,800 seconds with 20 managed
PlayerBots and one ordinary protocol player, then completed one controlled restart.
The ordinary session remained connected for 1,800.290 seconds before the restart,
sent six normal direction-turn activities, and was authoritatively placed again at
generation 2. Peak population was 20/20 with zero unexpected client exits, managed
ping timeouts, unexpected managed logins or logouts, session replacements, duplicate
sessions, or invariant failures. The run recorded 1,187,084 dispatcher samples;
RSS was 1,426,616 KiB initially, 1,437,864 KiB peak, and 1,420,920 KiB final.
Cleanup and all artifact checksums passed. Artifacts are under
`/home/playerbots/workspace/playerbots/logs/playerbots-soak/20260802-102146-1966231`.
This is regression smoke only: `PRODUCTION_SOAK=NO`, `RELEASE_SOAK=NOT_RUN`,
`M9E_COMPLETE=NO`, and `M9_COMPLETE=NO`.

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

## Qualifying release soak — 2026-08-02

The commit-bound release profile completed successfully from
`7c31d6046a7226423628be8826f4a7d407d4fb0e`.

Command parameters:

- profile: `release`
- duration: 7,200 seconds
- configured PlayerBots: 20
- ordinary protocol participants: at least one
- controlled restarts: three
- output:
  `/home/playerbots/workspace/playerbots/logs/playerbots-soak/20260802-163858-2081679`

Final results:

- result: `PASS`
- configured/peak managed population: 20/20
- human participants: 1
- peak ordinary players: 1
- ordinary-client placements: 4
- ordinary activities: 24
- maximum ordinary connected runtime: 7,201.141 seconds
- crossed 1,000-second idle boundary: yes
- controlled restarts: 3/3 `PASS`
- unexpected ordinary-client exits: 0
- managed ping timeouts: 0
- unexpected managed logouts/logins: 0/0
- unscheduled session replacements: 0
- duplicate sessions: 0
- invariant evaluations/failures: 7,523/0
- RSS KiB initial/peak/final:
  1,417,340 / 1,430,860 / 1,429,480
- dispatcher samples: 4,621,435
- dispatcher p50/p95/p99/max:
  50 / 100 / 250 / 323,754 microseconds
- cleanup: `PASS`
- private ordinary-client and database credential files: removed
- remaining soak processes: none

Final classification:

- `productionSoak=True`
- `releaseSoak=PASS`
- `PRODUCTION_SOAK=YES`
- `RELEASE_SOAK=PASS`

Artifact hashes:

- `SHA256SUMS`:
  `e8af6c829db6d5331a2dc40dc49ed1b83e68dc2c630ccd816988b0aed0cc23a2`
- `summary.json`:
  `362e1bc059cbf386ae405797a055739ff84a1f32a757e4dab5112e52d1978de4`
- `invariants.json`:
  `863698ceab6d71958a4e59f9fe823ffd672e21b750292d845cfb8a7a5be92adb`

`faults.json` is a reserved zero-byte artifact. The harness declares it in
the artifact manifest but has no writer or fault-document schema. Its empty
hash is covered by `SHA256SUMS`; it is not claimed as JSON fault evidence.
Fault coverage is provided by the named unit and integration tests.

The canonical gate passed with `BUILD_RC=0`, `UNIT_RC=0`,
`INTEGRATION_RC=0`, and `DIFF_RC=0`. PlayerBot unit tests passed 677/677,
integration tests passed 115/115, and PlayerBot tool tests passed 56/56.

The formal Codex review did not complete and remains
`INTERRUPTED_OR_NOT_RUN`. Therefore this evidence does not by itself set
`M9E_COMPLETE=YES` or `M9_COMPLETE=YES`, and OPS1A remains gated.
