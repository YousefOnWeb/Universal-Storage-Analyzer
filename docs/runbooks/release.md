# Release runbook (summary)

1. Clean configure + build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`.
2. `ctest --test-dir build --output-on-failure` — all labels green.
3. `UniversalStorageAnalyzer --smoke-test` and `--headless-check` exit 0.
4. Tag `vX.Y.Z`; `release.yml` builds per-OS artifacts, signs, attaches
   provenance, and uploads installers.
