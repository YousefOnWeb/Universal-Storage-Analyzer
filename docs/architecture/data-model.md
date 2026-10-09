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
absolute per-directory totals, propagated up the ancestor chain) and
renders through `FileSystemModel`, a `QAbstractItemModel` mirror fed by
`FileRecord` batches (≤500) plus absolute `BranchMetrics` summaries that
the model stores verbatim (never adds — adding would double-count).
Scan state is never shared across threads except through queued signals.
