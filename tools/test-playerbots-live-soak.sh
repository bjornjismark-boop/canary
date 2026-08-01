#!/usr/bin/env bash
set -Eeuo pipefail
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"
python3 -m unittest -v tests/tools/test_playerbots_live_soak.py
printf '%s\n' 'HARNESS_SELF_TEST=PASS' 'MODE=DETERMINISTIC_MOCKED_COMPONENTS' 'LIVE_SERVER=NO' 'MIXED_SOAK=NO'
