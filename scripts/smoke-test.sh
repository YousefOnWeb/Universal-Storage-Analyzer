#!/usr/bin/env bash
# Launch smoke test against an installed/mounted artifact.
set -euo pipefail
BIN="${1:-./build/src/UniversalStorageAnalyzer}"
"$BIN" --smoke-test
echo "SMOKE-OK"
