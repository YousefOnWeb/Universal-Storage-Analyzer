# Universal Storage Analyzer (FOSS)

High-performance, open-source storage intelligence for Windows, macOS, and Linux:
scan local drives, visualize consumption in a live dual-pane UI, and act on results.

> Sprint 1 scope: local directory scan → virtualized dual-pane UI → light/dark themes.
> Cloud protocols, charts, dedup, and exports arrive in Milestones 2–4.

## Quick start (Windows + MSYS2 UCRT64)

```powershell
# 1. Install Qt6 + toolchain (one time)
pacman -S --needed mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools `
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-gcc

# 2. Configure + build (from MSYS2 UCRT64 shell or with prefix on PATH)
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/tools/msys64/ucrt64" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 3. Verify
ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build -L integration --output-on-failure
./build/src/UniversalStorageAnalyzer --smoke-test
./build/src/UniversalStorageAnalyzer --headless-check
```

Linux/macOS: install `qt6-base` via your package manager and drop the
`-DCMAKE_PREFIX_PATH` override if Qt is on the default path.

## Layout

```text
src/app/        bootstrap (main.cpp, CLI modes)
src/domain/     Qt-free business rules (FSNode, FileTree)
src/services/   infrastructure (scanner worker, scheduler façade, SQLite)
src/models/     QAbstractItemModel projections (tree + details)
src/ui/         Widgets rendering (MainWindow, ThemeManager)
src/resources/  styles + migrations + Qt resource bundle
tests/          QtTest suites (CTest labels: unit, integration)
docs/           product / architecture / specs / runbooks / progress
scripts/        verify + smoke-test entry points
.github/        ci / release / nightly workflows
```

## Deterministic modes

| Flag | Purpose |
| --- | --- |
| `--smoke-test` | Init DB + services, print `SMOKE-OK`, exit 0 |
| `--headless-check` | Validate DB, migrations, resources, app-data dir |
| `--reset-test-data` | Clear test DB (requires `UNIVERSAL_STORAGE_ANALYZER_TEST=1`) |

## Docs

- Product: `docs/product/vision.md`, `user-flows.md`, `non-goals.md`
- Architecture: `docs/architecture/overview.md`, `module-boundaries.md`,
  `data-model.md`, `ipc-contracts.md`
- Spec: `docs/specs/001-sprint1-foundation.md`
- Progress: `docs/progress.md`
