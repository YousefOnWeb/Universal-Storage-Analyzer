# Data model

Node shape (one file or directory), as carried by `EngineNode`
(`src/services/scanner/ScanEngine.h`) and projected to the UI as
`FileRecord` (`src/services/ScanTypes.h`):

- `id` / `parentId` (`uint32_t`), `nameUtf8` (UTF-8 `std::string`)
- `logicalSize`, `physicalSize` (`uint64_t`; allocated is the real
  on-disk figure reported by the backend — allocation size, block-count
  derived, or data-run derived — not an estimate)
- `lastWriteTs` (Unix seconds), `isDirectory` flag

`EngineDirSummary` adds the aggregated `fileCount` / `folderCount` per
directory.

The running application aggregates in `ScanEngine` (per-run
absolute per-directory totals, propagated up the ancestor chain via
per-worker local maps merged into shared atomics at each flush) and
renders through `FileSystemModel`, a `QAbstractItemModel` mirror fed by
`FileRecord` batches (512 nodes / 250 ms, parent-sorted rechunk) plus
absolute `BranchMetrics` summaries that the model stores verbatim
(never adds — adding would double-count). Live summaries stream during
the scan; exact finals arrive as one bulk `onSweep` delivery applied
with a single coalesced repaint. Scan state is never shared across
threads except through queued signals.

Analytics ride the same pass: `Analytics` accumulates per-file
extension bytes/count (dot-suffixed, lowercased; `(no extension)` /
`(other)` over a 4096-entry cap), a largest-files min-heap (default
1000, Options range 100–10000; full paths converted lazily only for
heap members), age buckets over configurable day edges (default
`1,7,30,180,365`; unknown mtimes skipped), and per-owner bytes/count
(empty id → `(unknown)`). Snapshots (`AnalyticsSnapshot` →
`AnalyticsData`) are absolute replace-semantics values: live at most
~1 Hz plus one exact final. Panes render through `StatsTableModel`
(numeric sort keys, virtualized) with `QSortFilterProxyModel` sorting;
the tree filters by search text and `\.ext$` via `TreeFilterProxyModel`
(recursive parents kept).
