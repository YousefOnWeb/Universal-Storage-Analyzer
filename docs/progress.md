# Progress

## Completed (2026-10-09) — full revision pass over the scan engine

Fourteen atomic commits, each verified (build + full suite green before
commit). 9/9 suites green (3 unit + 5 integration + 1 ui), lint clean,
`--smoke-test`/`--headless-check` verified live.

- Correctness: NTFS `$FILE_NAME` allocated-size offset (+40, was reading
  flags) and standard sparse data-run decoding (`lcnBytes==0` emits a
  sparse run; VCN accounting no longer collapses across holes) — both with
  fixture tests. DirectMFT stays opt-in/fallback until proven on an
  elevated volume.
- Teardown: `ScannerService` destructor cancels before quit, deletes via
  the worker loop, bounded wait with terminate fallback (was: hang-prone
  `quit()+wait()` + never-delivered `deleteLater`); destroy-mid-scan test.
- Worker: batch signals assembled under lock, emitted outside it;
  atomic single-flight guard rejects overlapping runs (tested via
  pre-paused overlap).
- Hardening: `DirectoryFinder` memcpy field readers (alignment UB),
  `toLongPath` recursion guard, buffer reuse across opens (+relative-path
  test); lock-free pause/cancel flags; `ScanLogger` casts, honest
  batch/allocation lines, loud open failures, filename sanitizing; MFT
  volume cache validated by serial; `VirtualAlloc` RAII; truthful 2 GiB
  cap message.
- Removed: dead `FileTree`/`FSNode` domain container and its test
  (`ScanEngine` + `FileSystemModel` own scan state); docs updated.
- Models/UI: details selection clears synchronously on source reset
  (a mid-reset `QPersistentModelIndex` dies at `endResetModel`, and a
  post-reset one leaves proxies stale — plain index + clear is documented
  in the header); duplicated `clear()` lines removed; Ctrl+O/Ctrl+F/Ctrl+L
  shortcuts; scan target persisted; skip summary gains a next step.
- Build/CI: nightly `--offline` dropped (flag never existed); per-OS
  smoke paths; no committed machine-specific Qt prefix (pass
  `-DCMAKE_PREFIX_PATH` or `QT_PREFIX`); parent-console attach for CLI
  modes that preserves piped output; SHA-pinned actions; whitespace lint
  job; offscreen `test_ui` (`ui` CTest label) covering both panes and the
  end-to-end skip summary.
- Deferred with reasons (not silent): deep `loadVolume`/`getMftExtents`
  decomposition (elevation-gated, unverifiable here); full clang-format
  adoption (tool absent; lint is whitespace-level); signing/attestations.

## Completed (2026-10-04)

- Project initialized per concept/discovery, architecture, Sprint 1, and
  design/contract docs: layered `src/`, `tests/`, `docs/`, `scripts/`,
  CI workflows, README/CHANGELOG.
- Sprint 1 vertical slice implemented: portable scanner worker, tree/details
  models, dual-pane main window, theming, SQLite V1 migration, CLI modes.

## Decisions

- ADR 0001: portable traversal first (superseded: the fast path landed
  early, see ADR 0002).
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

- Merge `feature/scan-engine` into `main` (rebase, PR, green CI incl.
  lint/ui jobs), then delete `feature/scan-engine` + stale
  `feature/scan-logging`.
- Milestone 2: treemap/pie/bar charts + Extensions/Users/Age/Top analytics
  — written spec + approved plan before code (tabs already stubbed).
- Milestone 2: VFS + SSH/S3/Azure/WebDAV read paths behind `IScannerService`.
- Harden: pause/resume button, error rows for denied paths, export stubs live.
- Follow-ups: elevated DirectMFT proof scan; clang-format adoption;
  `loadVolume` decomposition; signing/attestations.
- Probe dig (2026-10-09): elevated scans reported 0 bytes for a fresh
  48-byte file while native read 48. Record dump proved `$FILE_NAME`
  real size stale (0) with resident `$DATA` current (48) — NTFS
  filename-size lag. Fixed by falling back to unnamed-stream data
  length when the filename size is zero (`$FILE_NAME` stays
  authoritative otherwise); proven on the live record, fixture-tested
  both branches. Still open: the probe itself fails on some C: temp
  paths (fallback notice seen in test runs) but succeeds on others —
  path-resolution vs volume-state cause unknown; the notice now carries
  the probe's reason to name it next time. Also open: ~7 s C:-volume
  image load cost per scan (shared cache helps only within a process).
