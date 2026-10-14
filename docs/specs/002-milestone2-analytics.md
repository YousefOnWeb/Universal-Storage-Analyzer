# Spec 002 — Milestone 2 analytics, visualization & adaptive engine

Goal: turn scan results into live, interactive insight — treemap, bars,
pie, and four analytics panes — plus an engine that throttles itself on
struggling disks and a settings dialog. Everything streams from the
existing single scan pass; nothing re-traverses the filesystem. F2 user
flow (`docs/product/user-flows.md`).

Performance is the top constraint, filesystem agnosticism second,
cross-platform parity third: every feature must work on Windows, macOS,
and Linux with no platform holding back the others. Where capture is
free on one backend but costly on another, the spec says exactly what
degrades and how the UI states it — never fake data.

## 1. In scope

1. Engine accumulators (one pass, incremental, bounded memory):
   extensions (bytes + count), global largest-files min-heap,
   age histogram over configurable buckets, per-owner bytes/count with
   opaque owner-id carry.
2. Treemap pane (squarified layout, 2D QPainter tiles, interactive).
3. Bars tab (top child directories) and Pie tab (volume distribution +
   free space), custom-painted, clickable drill-down.
4. Analytics panes: Extensions (with tree filter), Top Files (live),
   Users, Age.
5. Owner display-name resolution (lazy, cached, per-OS backend).
6. Disk telemetry sampler + engine throttling with hysteresis.
7. Options dialog (QSettings): age buckets, export prefs (M4 stub),
   startup target, top-N size.

## 2. Out of scope (stays deferred)

- 3D/hardware treemap mode → follow-on spec 003 reusing 002's layout
  output (2D ships first; QPainter with retained tile cache and partial
  repaints meets the perf budget — a GL dependency needs its own
  justification per AGENTS.md).
- QtCharts or any new third-party dependency: bars/pie are custom
  `QPainter` widgets (small, exact needs, no new link).
- VFS/network targets, dedup, mass ops, exports, scheduling (F3–F6).
- SQLite migration: all M2 settings live in `QSettings` (small prefs
  per AGENTS.md §2). The `age_intervals_days` seed stays as-is.
- QML anywhere (Widgets-only per AGENTS.md §1).

## 3. Architecture (must follow `module-boundaries.md`)

| Piece | Lives in | Rules |
| --- | --- | --- |
| Accumulators, squarify layout, histogram bucketing | `src/services/scanner/` (new `Analytics.*`, `TreemapLayout.*`) | Qt-free, unit-tested everywhere |
| Owner-name cache, telemetry sampler, throttle wiring | `src/services/` (`OwnerResolver.*`, `DiskTelemetry.*`) | QtCore only, no widgets |
| Pane/treemap/bar/pie models | `src/models/` (`QAbstractTableModel` + tile cache struct) | No FS/SQL/widgets |
| Treemap/bar/pie painter widgets, panes, options dialog | `src/ui/` | Main thread only, off-thread data via queued signals |
| Wiring | `src/app/main.cpp` | No logic |

New engine→bridge signals reuse the existing conventions: absolute
replace-semantics values, coalesced delivery (≤10 Hz for live lists),
`Qt::QueuedConnection` at every thread boundary, single-flight
unchanged.

## 4. Detailed behavior

### 4a. Engine accumulators (`Analytics.*`, Qt-free)

- Per file node, the engine updates: `extMap[lowerExt] += {bytes, 1}`
  (extension = substring after last `.`, lowercased, UTF-8; none →
  `"(no extension)"`; cap 4096 distinct, overflow → `"(other)"`);
  top-N min-heap keyed by logical size (N from options, default 1000,
  range 100–10000); age bucket index from `lastWriteTs` against the
  active bucket edges; `ownerMap[ownerId] += {bytes, 1}` (empty id →
  `"(unknown)"`, always shown honestly).
- Cost: O(1) amortized per file; memory bounded by caps above.
- Emission: accumulators ride the existing summary path — full snapshot
  with the final sweep (exact), plus a ≤1 Hz live snapshot while
  scanning (best-effort, same values, newer wins). No new thread.
- `EngineNode`/`DirEntry` gain `ownerId` (`std::string`, empty when the
  backend cannot provide it for free):
  - POSIX native: `lstat` uid, formatted as decimal string (free).
  - DirectMFT: `$STANDARD_INFORMATION` owner id (free).
  - Windows native: NO free owner — left empty (see 4e). Never an extra
    syscall per file in M2.

### 4b. Treemap pane

- Layout: squarified treemap over the **selected subtree's children**
  (not the whole volume): worst aspect ratio minimized per row,
  integer pixel rects, ≥3 px tiles labeled, smaller tiles fill only.
  `TreemapLayout.*` is pure Qt-free math: input `(id, weight)` list +
  rect → output tile rects. Deterministic given the same input.
- Rendering: `TreemapWidget` (QWidget + QPainter), retained tile cache
  (repaint only on resize/selection/epoch change — never per batch),
  hover highlight + tooltip (path, size, share %), click selects the
  node in the tree (both directions stay in sync), double-click
  descends selection into that folder.
- Freshness: layout cached per `(selectionId, epoch)`; recompute
  debounced 250 ms during live scans. Budget: ≤50 ms for 10k tiles.
- Placement: right side becomes a vertical splitter — tabs on top,
  treemap below, collapsible, sizes persisted in `QSettings`
  (`main/rightSplitter`). Default ratio ≈ 60/40.

### 4c. Bars & Pie tabs (custom QPainter, no QtCharts)

- Bars tab: top-20 children of the current tree selection by logical
  size, horizontal bars with exact-value labels, numeric sort, click a
  bar → select that node in the tree. Empty state text when the
  selection is a file.
- Pie tab: current volume (of the scan target) total vs free vs
  top-children slices via `QStorageInfo` + model data; click a slice →
  select that node; free-space slice is informational only. Legend with
  values, not color-only (accessibility).
- Both read the in-memory model only: zero I/O, instant on any scan.

### 4d. Analytics panes (replace placeholder tabs)

- Extensions: table (extension, bytes, bytes %, files), sorted by
  bytes desc. ~~Click a row → tree + details filter to `\.ext$`
  (case-insensitive, recursive parents kept); click again (or Esc in
  search) clears. `(no extension)` and `(other)` filterable the same way.~~
  REMOVED after review: picking a row no longer touches the tree —
  the pane is informational only (quick search still filters).
- Top Files: live global top-N (path, size, parent), updates ≤1 Hz
  during scans, exact at finish; click → select node in tree
  (expands parents).
- Users: table (owner display name, bytes, bytes %, files) from
  `ownerMap`; `(unknown)` row always present when non-zero, with a
  tooltip explaining why (backend-dependent, see 4e).
- Age: histogram over the configured buckets with counts + bytes;
  changing buckets in Options re-aggregates from in-memory mtimes
  (O(nodes), no I/O) and refreshes instantly.
- All panes: `QAbstractTableModel` + the existing proxy/sort-role
  pattern, virtualized, keyboard-navigable.

### 4e. Owner resolution (`OwnerResolver.*`, services layer)

- Transport is filesystem-agnostic: the engine carries opaque id
  strings only. Resolution to display names happens lazily in
  services, cached per unique id for the process lifetime:
  - Windows: `LookupAccountSidW` (handles domain accounts when joined;
    falls back to the raw SID string). No per-file I/O — resolution
    runs over distinct ids only (bounded, tiny).
  - POSIX: `getpwuid_r` on the numeric uid, fallback to `#uid`.
- Windows-native per-file owner *capture* is out of scope for M2
  (no free source; per-file `GetSecurityInfo` would dominate scan
  time). The Users pane states coverage honestly via the `(unknown)`
  row. Full capture is a follow-up pending measurement.

### 4f. Telemetry & throttling (`DiskTelemetry.*` + engine hook)

- Sampler: `QObject` worker on its own thread, 1 Hz poll, per-OS
  backend behind one interface — Windows PDH (Active Time %,
  Avg Queue Length, Read/Write MB/s of the target volume's physical
  disk), Linux `/proc/diskstats` (same three signals), macOS IOKit
  disk counters (same signals, best effort). Emits a plain struct;
  unsupported → reports `available=false` and the engine never throttles.
- Throttle policy (hysteresis, no flapping): engage after 3 consecutive
  samples with p95-ish latency >200 ms or sustained queue >4; release
  after 10 consecutive clean samples. Engage = stop dispatching NEW
  jobs and park excess workers (floor: 1); in-flight jobs always drain
  naturally. Release = restore up to `kMaxWorkers` gradually (+2/sec).
- Thread-safety mirrors pause/cancel: atomics + queue primitives only,
  invokable via direct connection mid-scan. Status bar shows
  `Throttled (disk busy)` while engaged; transitions logged to the
  session log and the per-scan log.
- Default ON for rotational media, ON but rarely triggering for SSDs;
  toggle in Options (`options/throttle`, default true).

### 4g. Options dialog

- Modal `QDialog`, `Options` menu entry + toolbar button: age buckets
  (CSV days, validated: 1–3650, ascending, ≤12 edges, instant
  re-aggregation), top-N size (100–10000), startup target
  (`last|home|ask` — extends the persisted `main/target`), export
  format selector (disabled stub noting Milestone 4), throttle toggle.
- Persistence: `QSettings` only (`options/*`). Validation errors are
  inline (no popups for fixable input).

## 5. Acceptance criteria

- [ ] Fresh scan: treemap renders the target's children within 1 s of
      finish; tiles match tree totals exactly (tile sum == node total).
- [ ] Live scan: panes/treemap update without freezing (existing
      coalescing budgets hold; no new direct widget writes off-thread).
- [ ] Extensions sum == scan totals; top-N list == true top-N of a
      fixture tree; age buckets match an independent recount; owner
      map exact where ids are captured.
- [ ] ~~Extension filter shows exactly the matching files (+ ancestor
      chain) and clears completely.~~ REMOVED after review: picking an
      extension row no longer filters the tree (spec 002 §4d
      amendment below); the panes stay purely informational.
- [ ] Bars/Pie clicks select the right tree node; Pie free-space
      matches `QStorageInfo` within one refresh.
- [ ] Throttle: synthetic sampler (over-threshold fixture) parks the
      pool to 1 and restores after clean samples; status text flips.
- [ ] Options round-trip across restarts; invalid input rejected inline.
- [ ] Perf: accumulator overhead <5% vs uninstrumented engine on the
      existing bench shape; treemap ≤50 ms @10k tiles; top-N emit ≤1 Hz.
- [ ] `unit` + `integration` + `ui` green; lint clean; `--smoke-test` /
      `--headless-check` exit 0; no new warnings; layering grep clean
      (`QtWidgets|QWidget` absent outside `src/ui`, no `#include <Q`
      in `src/services/scanner`).

## 6. Vertical slices (each: plan → implement → verify → commit)

- 002a accumulators + owner-id carry (engine only; no UI). Tests:
  fixture trees asserting exact ext/top/age/owner maps.
- 002b layout math + treemap pane + splitter persistence. Tests: layout
  unit tests (aspect bounds, determinism, area conservation); ui test
  (renders, click↔tree sync both ways).
- 002c Bars + Pie tabs. Tests: model-driven values; ui click-drill test.
- 002d four analytics panes + extension filter. Tests: filter exactness;
  ui toggle test.
- 002e owner resolution + Users honesty states. Tests: resolver cache
  unit tests (fake ids); ui `(unknown)` presence test.
- 002f sampler + throttle + status indicator. Tests: synthetic-sampler
  integration (park/restore); hysteresis unit tests.
- 002g options dialog + persistence + re-aggregation. Tests: validation
  unit tests; ui round-trip test (QSettings sandbox via
  `QSettings::setPath` in test setup).

## 7. Deliberately unchanged

Scan pipeline and batching, error-severity contract, CLI modes (they
respect options but gain no charts), Details columns, DB schema,
packaging/signing. `docs/product/non-goals.md` updates per slice as
placeholders go live.

> Post-approval perf amendments (landed on `feature/m2-analytics`,
> within the §5 perf budget — not scope changes): batch cadence is
> 512 nodes / 250 ms with parent-sorted rechunk (not small ungrouped
> flushes); per-worker ancestor aggregation merged at flush; final
> directory totals delivered as one bulk sweep event; top-N paths
> converted lazily for heap members only; no finish-time re-sort
> (proxies stay dynamically unsorted from scan start; header clicks
> sort on demand).
