# Progress

## Completed (2026-10-04)

- Project initialized per concept/discovery, architecture, Sprint 1, and
  design/contract docs: layered `src/`, `tests/`, `docs/`, `scripts/`,
  CI workflows, README/CHANGELOG.
- Sprint 1 vertical slice implemented: portable scanner worker, tree/details
  models, dual-pane main window, theming, SQLite V1 migration, CLI modes.

## Decisions

- ADR 0001: portable `std::filesystem` traversal first; MFT fast path later.
- ADR 0002: dual-backend enumeration (`Finder` + direct volume reader with
  automatic native fallback) coordinated by `ScanEngine` over a
  `BlockingQueue` pool; engine owns aggregation, model stores verbatim
  (fixes latent double-count of directory totals).
- Error severity is decided at the engine source: fatal (scan cannot
  proceed) pops up immediately; per-directory skips (108/sec observed on
  real volumes) are collected and shown once at scan end.
- Session telemetry scope (deliberate): JSON telemetry lines (periodic +
  final) with scan/thread/subsystem IDs, rates, queue depth, and backend;
  OS error codes on enumerate failures; ISO-8601 UTC session timestamps.
  Explicitly deferred to later milestones: async log ring buffer (current
  volume is ~4 lines/sec, no hot-path logging exists), path redaction
  (no export bundles yet), physical disk monitor (needs platform APIs +
  an adaptive engine that does not exist yet), cloud/dedup telemetry (no
  such subsystems exist yet), per-file TRACE, microsecond resolution
  (Qt/OS clock limits).
- Measured: native backend sustains ~500k nodes/sec while the direct
  reader managed ~11k — the gap is per-directory path resolution
  (O(depth x width) string descent per job), not disk I/O. Fixed by
  carrying the backend record token through scan jobs for O(1) opens.
  Follow-up measurement after the fix: engine-side 82k nodes/sec with the
  GUI thread drowning in ~2M summary signals, so summaries now flow
  leaf-only per flush with a final exactness sweep, model view updates
  coalesce at 10 Hz, and live resorting pauses during scans.

## Next tasks

- Milestone 2: treemap/pie/bar charts + Extensions/Users/Age/Top analytics.
- Milestone 2: VFS + SSH/S3/Azure/WebDAV read paths behind `IScannerService`.
- Harden: pause/resume button, error rows for denied paths, export stubs live.
