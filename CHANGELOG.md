# Changelog

All notable user-facing changes are recorded here.

## [0.1.0] — 2026-10-04

### Added

- Sprint 1 foundation: local directory scan with background worker thread.
- Dual-pane main window (file-system tree + Details tab) with live updates.
- Details columns: Name, Size, Allocated, Files, Folders, Last Modified.
- Quick Search filtering, size-unit selector, drive/free-space status bar.
- System/Light/Dark theming persisted via QSettings.
- SQLite store with `V1__Initial_Schema` migration (settings, snapshots, tasks).
- Deterministic modes: `--smoke-test`, `--headless-check`, `--reset-test-data`.
