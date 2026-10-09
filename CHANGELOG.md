# Changelog

All notable user-facing changes are recorded here.

## [Unreleased]

### Added

- Headless `--cli-scan <path>` mode with live progress, tagged errors,
  and final summary on stdout (`--no-log` skips the log file).
- Per-scan diagnostic logs with backend metadata, duration/throughput/
  batch/error stats, host block, and full paths in error messages.
- Session debug logs (ISO-8601 UTC, thread-safe) and machine-parsable
  scan telemetry (periodic + final JSON records).
- Error severity: fatal errors pop up immediately; per-directory skips
  are collected into one end-of-scan summary dialog.
- Numeric sort role: sorting compares raw integers, including Last Modified.
- Expert shortcuts: Ctrl+O select target, Ctrl+F find, Ctrl+L focus target.
- Scan target folder persisted across launches.
- Skip summary suggests next steps (re-run elevated, exclude system folders).

### Changed

- Core scanning is now multi-threaded with automatic backend selection:
  direct volume reading when elevated on a supported volume, native
  directory enumeration otherwise. Same UI, same results, higher
  throughput; pause/cancel now take effect immediately mid-scan.
- Directory opens use backend record tokens (O(1)) instead of path
  descent; model indexes resolve in O(1) via stored rows.
- Summary delivery throttled (leaf reports + final sweep) with coalesced,
  per-tick-capped view notifications; live resorting pauses during scans.

### Fixed

- Directory totals no longer double-count (engine owns aggregation).
- Fail-fast crash on volumes with sparse volume regions (image sized by
  span, holes zero-filled).
- Fail-fast crash on names with unpaired surrogates (infallible converter;
  shared log writes serialized).
- SQLite duplicate-connection warning on repeated initialization.
- NTFS record parsing: allocated size and sparse data runs decoded per
  spec (multi-extent fallback and hole accounting corrected).
- Shutdown during an active scan no longer blocks; overlapping scans are
  rejected with a non-fatal error instead of sharing engine state.
- Details pane no longer follows freed nodes across scan resets.
- CLI modes attach to the parent console when stdout has nowhere to go.

## [0.1.0] — 2026-10-04

### Added

- Sprint 1 foundation: local directory scan with background worker thread.
- Dual-pane main window (file-system tree + Details tab) with live updates.
- Details columns: Name, Size, Allocated, Files, Folders, Last Modified.
- Quick Search filtering, size-unit selector, drive/free-space status bar.
- System/Light/Dark theming persisted via QSettings.
- SQLite store with `V1__Initial_Schema` migration (settings, snapshots, tasks).
- Deterministic modes: `--smoke-test`, `--headless-check`, `--reset-test-data`.
