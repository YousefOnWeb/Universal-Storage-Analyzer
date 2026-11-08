# Universal Storage Analyzer

High-performance, open-source storage intelligence for Windows, macOS, and Linux:
scan local drives, visualize consumption in a live dual-pane UI, and act on results.

> Current scope: dual-backend scan engine (direct volume reading when
> elevated, portable enumeration otherwise) → virtualized dual-pane UI with
> treemap/bars/pie + Extensions/Top Files/Users/Age analytics → light/dark
> themes → per-scan logs + session telemetry → headless CLI → disk-busy
> throttle + options dialog.
> Milestone 2 analytics are implemented on `feature/m2-analytics`
> (unreleased); cloud protocols, dedup, and exports arrive in
> Milestones 3–4.

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
src/app/        bootstrap (main.cpp, CLI modes, session logging)
src/domain/     reserved for Qt-free business rules (currently empty:
                scan state lives in ScanEngine + FileSystemModel)
src/services/   infrastructure (scan engine + Analytics/ThrottlePolicy/
                TreemapLayout, Qt bridge, OwnerResolver, DiskTelemetry,
                scan logger, SQLite)
src/models/     QAbstractItemModel projections (tree + details +
                StatsTableModel + TreeFilterProxy)
src/ui/         Widgets rendering (MainWindow, ThemeManager, TreemapWidget,
                BarWidget, PieWidget, OptionsDialog)
src/resources/  styles + migrations + Qt resource bundle
tests/          QtTest suites (CTest labels: unit, integration, ui)
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
| `--cli-scan <path>` | Headless scan with live progress + summary on stdout |
| `--no-log` | With `--cli-scan`: skip the per-scan diagnostic log file |

## Scan logs, session logs & telemetry

- **Per-scan logs** (`<AppData>/scan-logs/scan_*.log`, gitignored): target,
  volume (filesystem, total/free/available, block size, read-only), engine
  block (backend used, files/dirs/bytes, duration, throughput, batches,
  errors, cancellation), host block.
- **Session logs** (`<AppData>/session-logs/session-*.log`): every Qt
  message with ISO-8601 UTC timestamps, thread-safe across workers.
- **Scan telemetry**: the engine emits machine-parsable JSON lines —
  periodic snapshots (scan/thread/subsystem IDs, rates, queue depth,
  backend) plus a final summary — captured into the session log.
  Enumerate failures carry OS error codes (`win32=`, `errno=`).

## Docs

- Product: `docs/product/vision.md`, `user-flows.md`, `non-goals.md`
- Architecture: `docs/architecture/overview.md`, `module-boundaries.md`,
  `data-model.md`, `ipc-contracts.md`
- Spec: `docs/specs/001-sprint1-foundation.md`, `docs/specs/002-milestone2-analytics.md`
- Progress: `docs/progress.md`
