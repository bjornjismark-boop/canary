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

The verified Linux release build uses the repository preset with unity disabled
because the current PlayerBot sources contain translation-unit-local helper
names that collide in a unity source:

```sh
cmake --preset linux-release -DSPEED_UP_BUILD_UNITY=OFF
cmake --build --preset linux-release --target canary -j6
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

## Final release soak

The roadmap currently does not approve a finite duration or release population.
Those two values must be approved before execution. The exact next command is:

```sh
tools/run-playerbots-live-soak.sh \
  --env /home/playerbots/.config/playerbots/canary-test.env \
  --config /absolute/path/to/test-config/config.lua \
  --server /home/playerbots/workspace/playerbots/canary/canary \
  --profile release \
  --duration-seconds <ROADMAP_REQUIRED_DURATION> \
  --bots <APPROVED_RELEASE_POPULATION> \
  --restarts 3 \
  --output-root /home/playerbots/workspace/playerbots/logs/playerbots-soak
```

Do not replace the placeholders or set `PRODUCTION_SOAK=YES` until duration and
scale are approved and a live Canary plus ordinary protocol client genuinely run.
