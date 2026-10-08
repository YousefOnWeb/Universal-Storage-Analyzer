# Data model

`FSNode` (`src/domain/FSNode.h`) — one file or directory:

- `id` / `parentId` (`uint32_t`), `name` (UTF-8 `std::string`)
- `logicalSize`, `allocatedSize` (`uint64_t`; allocated is the real
  on-disk figure reported by the backend — allocation size, block-count
  derived, or data-run derived — not an estimate)
- `fileCount`, `folderCount` (aggregated on directories)
- `lastModifiedTs` (Unix seconds), `isDirectory` / `isCompressed` bit-flags
- `children` (`vector<uint32_t>`)

`FileTree` is a tested domain-side container, but it is **not** on the
live path: the running application aggregates in `ScanEngine` (per-run
absolute per-directory totals, propagated up the ancestor chain) and
renders through `FileSystemModel`, a `QAbstractItemModel` mirror fed by
`FileRecord` batches (≤500) plus absolute `BranchMetrics` summaries that
the model stores verbatim (never adds — adding would double-count).
The domain object is never shared across threads.
