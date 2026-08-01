# PlayerBots live-soak operator guide

The live harness is prerequisite infrastructure. A smoke result is not M9E
release evidence and always records `PRODUCTION_SOAK=NO`.

## Safety contract

Use a dedicated host and an explicitly disposable database. The harness refuses
non-loopback `config.lua`, ambiguous database identities, a missing executable,
unbounded duration/population, and release profiles with fewer than three
restarts. It records and signals only the child PID it launched, and verifies
the Linux `/proc` start-time identity before every RSS sample and signal.

The environment file must be mode 0600 and provide:

```text
TEST_DB_NAME=playerbots_soak_unique
TEST_DB_ALLOW_RESET=1
SOAK_CLIENT_COMMAND=/absolute/path/to/ordinary-protocol-client --env-file /absolute/path/to/client.env
```

Use the existing `TEST_DB_HOST`, `TEST_DB_USER`, `TEST_DB_PASSWORD`,
`TEST_DB_PORT`, and `TEST_DB_SCHEMA` keys. The harness creates a unique database,
loads the repository schema and pre-launch fixtures, and drops it after shutdown.
Credentials are passed through a private temporary MySQL client file, not command
arguments. The ordinary-client command must authenticate, enter the world, remain connected,
send harmless protocol heartbeats, reconnect after a server restart, and exit
nonzero on an unexpected disconnect. It must never use BotManager.

Canary has no `--config` option. The harness creates a private isolated runtime
directory, copies the explicit template to `config.lua`, appends loopback ports
and disposable database values, generates `config/playerbots.json`, and links
only required repository data directories. It never edits the source config.

`PLAYERBOTS_SOAK_OUTPUT` enables the server-side local adapter. The harness sets
it to the private run directory. The adapter has no listener, is disabled by
default, copies M9D snapshots, consumes bounded JSONL requests through M9C, and
exports an independent bounded dispatcher histogram. Never set it manually for
a production server.

## Smoke

```sh
tools/run-playerbots-live-soak.sh \
  --env /home/playerbots/.config/playerbots/canary-test.env \
  --config /absolute/path/to/test-config/config.lua \
  --server /home/playerbots/workspace/playerbots/canary/canary-debug \
  --profile smoke \
  --duration-seconds 90 \
  --bots 4 \
  --output-root /home/playerbots/workspace/playerbots/logs/playerbots-soak
```

## Final release soak

The roadmap currently does not approve a finite duration or release population.
Those two values must be approved before execution. The exact next command is:

```sh
tools/run-playerbots-live-soak.sh \
  --env /home/playerbots/.config/playerbots/canary-test.env \
  --config /absolute/path/to/test-config/config.lua \
  --server /home/playerbots/workspace/playerbots/canary/canary-debug \
  --profile release \
  --duration-seconds <ROADMAP_REQUIRED_DURATION> \
  --bots <APPROVED_RELEASE_POPULATION> \
  --restarts 3 \
  --output-root /home/playerbots/workspace/playerbots/logs/playerbots-soak
```

Do not replace the placeholders or set `PRODUCTION_SOAK=YES` until duration and
scale are approved and a live Canary plus ordinary protocol client genuinely run.
