#!/usr/bin/env bash
# Full verification: lint → compile → unit → integration → ui → smoke.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
bash "$ROOT/scripts/lint.sh"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" "$@"
cmake --build "$BUILD" --config "${BUILD_TYPE:-Release}"
ctest --test-dir "$BUILD" -L unit --output-on-failure
ctest --test-dir "$BUILD" -L integration --output-on-failure
ctest --test-dir "$BUILD" -L ui --output-on-failure
"$BUILD/src/UniversalStorageAnalyzer" --smoke-test
"$BUILD/src/UniversalStorageAnalyzer" --headless-check
echo "VERIFY-OK"
