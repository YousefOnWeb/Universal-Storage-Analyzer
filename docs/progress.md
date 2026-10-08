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
  GUI thread drowning in ~2M summary signals. Coalescing bounds frequency
  but not count, so forced-flush summaries were removed outright (the
  final sweep guarantees them), per-tick emissions capped at 2000 for
  smooth catch-up, model view updates coalesce at 10 Hz, and live
  resorting pauses during scans. Bench: 48k vs 288k emissions in the
  same drain window.
- Crash fix (fail-fast 0xc0000409 at scan start, confirmed via WER fault
  bucket): sparse MFT runs advanced the VCN walk without emitting
  extents, so later fileOffsets overshot the summed image buffer — heap
  overflow on volumes with sparse MFT regions. The image is now sized by
  span (holes stay zero-filled, which is also what sparse clusters
  contain). Verified by review + suite; needs one elevated scan to
  confirm on the affected volume.
- Crash fix #2 (same fail-fast signature, reproduced live): NTFS allows
  unpaired surrogates in names; std::filesystem converters throw on them,
  and a throw in an engine worker thread terminates the process ~1s into
  any scan touching such a name. Fixed with an infallible lossy UTF-16
  converter (lone units become U+FFFD) plus a mutex around the shared
  session-log file handle. Proven by a full C:\ scan completing
  (1.66M files, 315 collected errors) that previously died in ~1s.
- Model updates: batched row inserts by parent, numeric sort role
  (sorts compare int64s — 2.2 s on 2M rows vs guaranteed hang before),
  coalescing cap kept generous (2000/tick) so the end-of-scan sweep
  drains in seconds, not minutes.

## Next tasks

- Milestone 2: treemap/pie/bar charts + Extensions/Users/Age/Top analytics.
- Milestone 2: VFS + SSH/S3/Azure/WebDAV read paths behind `IScannerService`.
- Harden: pause/resume button, error rows for denied paths, export stubs live.
