#!/usr/bin/env bash
# Full verification: format → compile → unit → integration → smoke.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" "$@"
cmake --build "$BUILD" --config "${BUILD_TYPE:-Release}"
ctest --test-dir "$BUILD" -L unit --output-on-failure
ctest --test-dir "$BUILD" -L integration --output-on-failure
"$BUILD/src/UniversalStorageAnalyzer" --smoke-test
"$BUILD/src/UniversalStorageAnalyzer" --headless-check
echo "VERIFY-OK"
