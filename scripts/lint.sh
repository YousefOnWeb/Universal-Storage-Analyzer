#!/usr/bin/env bash
# Lint: trailing whitespace, tabs in C++ sources, missing final newline.
# Runs anywhere bash + git exist (CI lint job, scripts/verify.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
fail=0

# Trailing whitespace in tracked text files (binaries: none tracked).
if git grep -n ' $' -- src tests docs scripts .github CMakeLists.txt README.md CHANGELOG.md AGENTS.md 2>/dev/null | grep .; then
    echo "LINT-FAIL: trailing whitespace above"
    fail=1
fi

# Tabs in C++ sources (repo uses spaces).
if git grep -n -e "$(printf '\t')" -- 'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h' 2>/dev/null | grep .; then
    echo "LINT-FAIL: tabs above"
    fail=1
fi

# Missing final newline in tracked sources and scripts.
while IFS= read -r f; do
    [ -n "$f" ] || continue
    if [ -n "$(tail -c 1 "$f")" ]; then
        echo "LINT-FAIL: missing final newline: $f"
        fail=1
    fi
done < <(git ls-files 'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h' 'scripts/*' 'CMakeLists.txt' 'src/CMakeLists.txt' 'tests/CMakeLists.txt' '.github/workflows/*.yml')

if [ "$fail" = 0 ]; then
    echo "LINT-OK"
fi
exit "$fail"
