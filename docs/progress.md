# Progress

## Current (2026-10-11) — branch `feature/m2-analytics` at `da8bf33`

- Milestone 2 analytics (spec 002) implemented and verified on this
  branch (unreleased, unmerged): engine accumulators, treemap/bars/pie,
  four analytics panes, owner resolution, telemetry + throttle, options
  dialog. 14/14 suites green (7 unit + 6 integration + 1 ui), lint
  clean, `--smoke-test` / `--headless-check` exit 0.
- Post-M2 perf pass (same branch, after the M2 completion note below):
  batches 512 nodes / 250 ms with parent-sorted rechunk (was 128 /
  100 ms + per-dir forced flushes); finish-time re-sort removed
  (saves ~2.8 s on 2M rows — proxies stay unsorted live, header clicks
  sort on demand); final directory totals as one bulk `onSweep` event
  (was one queued signal per directory); top-N full paths converted
  lazily only for heap members; ancestor totals aggregated per worker
  and merged at flush. Measured C: 52.6 s → 31.2 s click-to-Ready.
- Spec 003 (MFT streaming) was drafted and prototyped, then reverted
  (`git reset --hard da8bf33`): streaming reader + single map +
  `DirectMftFinder` never engaged live (volume-path and root-record
  bugs, fell back to `NativeDirectory`). No streaming files remain in
  the tree; `MftFinder` (image + triple map) is still the DirectMft
  backend. A future retry starts from spec-first, not from the
  reverted code.

## Current (2026-10-12) — spec 004 slice 004a (audit instrumentation)

- `ScanEngine` captures size-audit evidence with zero behavior change:
  every zero-logical file named with backend + physical size (capped
  2000, counted unbounded), reparse listings counted, live chain
  reports counted per directory, sweep-vs-chain deltas counted with
  abs byte delta, root subtree-max mtime tracked. `EngineResult`
  carries the counters + capped entries + root max mtime;
  `ScanLogger` writes a `[SizeAudit]` section plus footer provenance
  (`Total bytes` labeled as logical-size sum; free/available with
  volume + timestamp).
- New test `sizeAuditCapturesZeroLogicalFiles` (empty + normal file
  fixture) green. Suite status: 13/14 — `test_ui` crashes with no
  output on the clean tree too (pre-existing, unrelated to 004a;
  verified via stash + rebuild). Next: operator runs one elevated
  scan and returns the `[SizeAudit]` section; 004b fixes follow from
  that evidence, not guesses.

## Current (2026-10-12) — spec 004 slice 004b (native re-stat for zero-record sizes)

- Audit evidence (elevated DirectMft proof scan, 1.96M records):
  16,661 zero-logical files; `pagefile.sys` logical 0 / allocated
  ~14.9 GB, `hiberfil.sys` 0 / ~6.8 GB, `swapfile.sys` 0 / 16 MB,
  report `.txt` files 0 / 0, VM disk images 0 / 0; 904 reparse
  listings; `errors=0`. Verdict: record size fields are zero/stale
  for locked system files and multi-extent streams — not a counting
  bug, a size-source gap (spec §4b path).
- Fix: `MftFinder::findNext` re-stats zero-logical file leaves via
  `GetFileAttributesExW` (opens nothing, locks do not matter) and
  adopts the live size only when nonzero; reparse points and
  directories never re-statted. New `DirEntry::sizeRestated` flag;
  engine counts fixes into `EngineResult::restatFixed`; audit still
  names every file that remains zero. Helper exposed as
  `usa::scanner::nativeRestatSize` (Windows-only) with a
  no-elevation unit test (live temp-file size, directory refusal,
  missing-path failure).
- Suite status: 14/14 green (the earlier `test_ui` crash was a stale
  binary from the stash dance — full rebuild passes). Next: operator
  runs one elevated scan; `RestatFixed` + shrunken `ZeroLogical`
  list prove the fix, and the remaining zero list scopes any
  extension-record follow-up.

## Current (2026-10-12) — spec 004 slice 004b2 (dirent fallback for locked files)

- Proof scan with 004b (DirectMft, 1.96M records): total 321.95 →
  398.18 GiB (+76 GiB); `RestatFixed: 1979`; report `.txt` files and
  the VM `.vmdk` repaired. Still zero: `pagefile.sys` (alloc ~14.9
  GB), `hiberfil.sys` (~6.8 GB), `swapfile.sys` (16 MB) — attribute
  stats fail on exclusively locked files too — plus 14,680
  allocated-0 files (lock/marker/placeholder pattern, plausibly
  genuinely empty) and one `.vmsd`.
- Fix: second-level fallback gated on `physicalSize > 0` (giants only,
  empty files cost nothing): enumerate the parent directory and match
  by name, adopting the entry size when nonzero. New
  `usa::scanner::nativeDirentSize` (Windows-only) with a no-elevation
  unit test. Expectation: ~45 GB of the remaining gap closes; the
  0/0 remainder is then either genuinely empty or individually named
  in the audit for the next slice.

## Current (2026-10-12) — 004b2 proven by elevated scan

- Proof scan with 004b2 (DirectMft, 1.96M records, `RestatFixed:
  1982`): total 398.18 → **443.60 GiB** (+45.4 GiB, exactly the
  locked-file class); `pagefile.sys` / `hiberfil.sys` / `swapfile.sys`
  gone from the zero list; `ZeroLogical: 14680` (reparse listings +
  allocated-0 files). Volume used space is 446.5 GiB, so the logical
  sum now covers 99.4% of it — totals are volumetrically reconciled.
- Remaining zero list: one `.vmsd` (allocated 0, likely genuinely
  empty — operator spot-check pending), lock/marker/placeholder
  files, junction points (spec 005 grouping). `.m4a` call recordings
  at 0/0 need one Properties spot-check to rule out hidden data.
- Next slice 004c (counting rules): −51k files / +5.6k folders vs
  reference — hardlink/multi-name records, junction descent and
  counting, per-gap-directory deltas. Needs GUI totals for the gap
  directories before implementation.

## Current (2026-10-12) — 004b proven complete by spot-check + rescan

- Operator Properties check on two `Call recording *.m4a` files
  (audit: logical 0 / allocated 0): real sizes 925 KB and 144 KB.
  Neither appears in any post-004b zero list — level-1 re-stat
  recovers ordinary readable files completely. The 0/0 remainder is
  therefore genuinely-empty files, reparse points, or locked files
  with allocated 0; no third size-source class.
- Rescan stability (same build, same volume, minutes apart):
  443.60 → 443.85 GiB (+747 KB drift = live volume writes);
  `RestatFixed: 1984`; `errors=0`. First scan of the session ran
  slower (83 s engine, cold OS/volume caches) than the second
  (26 s) — cache warmth, not a code path difference (`throttled:
  false` throughout, same backends and counts).
- Size-source work (spec §4a–§4b) is done. Only 004c counting rules
  remain before the numbers are fully reconciled.

## Current (2026-10-12) — spec 004 slice 004c (one row per hardlink)

- Gap analysis from operator GUI totals (reference GB read as GiB):
  ProgramData reconciled (−1.9%, ±4 rows); VM folder reconciled to
  0.1% (sparse image: true clusters vs reference repeating logical
  into Allocated); Windows −41,511 files × ~174 KB avg = −7.2B bytes,
  exactly the missing hardlink names. Reference Allocated always
  equals its Size, so it is not measured — ours stands. The +5.6k
  folders on our side is volume-reader coverage (access-denied
  subtrees enumeration skips), documented, not dropped.
- Fix: `MftRecord` keeps every `$FILE_NAME` link; `buildChildLinks`
  expands one `ChildRef` per real name (DOS aliases shadowed, DOS-only
  records still list once); `findNext`/`open` resolve each row under
  its linked name. New `EngineResult::multiNameExtra` + `NameLinks:`
  audit line from the probe load. Tests: multi-name fixture parse +
  link-expansion matrix (both green); full suite 14/14, lint clean.
- Next: operator runs one elevated scan; Windows files should land
  near 199k with `NameLinks` ≈ 40–60k, closing both counts and bytes.

## Current (2026-10-12) — spec 004 accepted on proof scan

- Proof scan with 004c (DirectMft, 2.02M rows): `NameLinks: 55579`
  (predicted 40–60k); files 1,643,271 → **1,699,710** (reference
  1,693,976: **+0.34%**); total 443.85 → **458.13 GiB** (reference
  461.5 GiB, units confirmed GiB via the VM row: 44.93B bytes =
  41.84 GiB printed as 41.8: **−0.73%**). Folders +1.8%, sourced to
  volume-reader coverage of access-denied subtrees (documented rule).
  The systematic −41k/−7.2B Windows gap is closed; residuals are
  volume churn between captures plus unit/timing noise.
- Watch item (not a gate): traverse 27.8 → 46.5 s on this run with
  identical backends and `throttled: false`; 004c adds no hot-path
  cost (expansion runs once at load), so this reads as environmental
  (AV/update/indexer). Next scan timing decides whether to profile.
- Spec 004 acceptance oracle (§4g/§5) is met: bytes and files within
  1% of reference, folders individually sourced, zero list named,
  audit complete. Remaining specs 005 (hierarchy/selection/sorting)
  and 006 (columns/formatting/summaries) own the UI-restructuring
  half of the comparison.

## Completed (2026-10-09) — Milestone 2 analytics (spec 002, branch `feature/m2-analytics`)

- Engine accumulators in the single scan pass (extensions, top-N heap,
  age histogram, owner carry; <5% budget, bounded memory), live ≤1 Hz
  snapshots plus exact finals.
- Treemap pane (squarified Qt-free layout, retained cache, debounced
  live refresh), Bars/Pie tabs (custom paint, no QtCharts), four
  analytics panes with extension filtering and drill-down, owner
  display names with lazy cache, disk telemetry + hysteresis throttle
  with status display, options dialog (QSettings, live age
  re-bucketing, startup rules).
- Probe chase (elevated): stale `$FILE_NAME` sizes and DOS-alias-first
  records fixed with live-record proof; downgrade notices moved out of
  the error channel; mtime-gated volume cache; backend assertions
  follow the OS gate.
- 14/14 suites green (unit/integration/ui), lint clean, smoke/headless
  verified. Next: merge, then spec 003 (3D treemap) or VFS targets.

## Completed (2026-10-09) — full revision pass over the scan engine

Fourteen atomic commits, each verified (build + full suite green before
commit). 14/14 suites green (7 unit + 6 integration + 1 ui), lint clean,
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

- Merge `feature/m2-analytics` into `main` (rebase, PR, green CI incl.
  lint/ui jobs), then delete the branch + stale `feature/scan-engine`
  / `feature/scan-logging` if still present.
- Milestone 3: VFS + SSH/S3/Azure/WebDAV read paths behind `IScannerService`.
- 3D treemap (follow-on to spec 002 §2, needs its own justification
  per AGENTS.md) or MFT streaming retry (spec-first; the reverted
  003 prototype is not a base).
- Harden: pause/resume button, error rows for denied paths, export stubs live.
- Follow-ups: elevated DirectMFT proof scan; clang-format adoption;
  `loadVolume` decomposition; signing/attestations.
- Probe dig (2026-10-09): elevated scans reported 0 bytes for a fresh
  48-byte file while native read 48. Record dump proved `$FILE_NAME`
  real size stale (0) with resident `$DATA` current (48) — NTFS
  filename-size lag. Fixed by falling back to unnamed-stream data
  length when the filename size is zero (`$FILE_NAME` stays
  authoritative otherwise); proven on the live record, fixture-tested
- Probe dig part 2: fallback with "path component not found" on fresh
  `test_scanner-*` dirs, always the leaf. Root cause: fresh long-named
  entries carry the 8.3 DOS alias FIRST, and the parser took it, so
  lookup compared "TEST_S~1" against the long name. Fixed by preferring
  non-DOS namespaces (alias kept as last resort); proven live — the
  same suite now scans via DirectMFT with no fallback. Remaining cost
  note: each fresh-dir scan reloads the whole volume image (~7 s on
  this 2M-record volume); steady rescans stay cached.
