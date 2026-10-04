# Data model

`FSNode` (`src/domain/FSNode.h`) — one file or directory:

- `id` / `parentId` (`uint32_t`), `name` (UTF-8 `std::string`)
- `logicalSize`, `allocatedSize` (`uint64_t`; allocated = 4 KiB-aligned)
- `fileCount`, `folderCount` (aggregated on directories)
- `lastModifiedTs` (Unix seconds), `isDirectory` / `isCompressed` bit-flags
- `children` (`vector<uint32_t>`)

`FileTree` owns nodes in an `unordered_map`, assigns ids, and propagates
size/count deltas to ancestors under a mutex. The live UI keeps its own
`FileSystemModel` mirror fed by `FileRecord` batches (500/batch) so the
domain object is never shared across threads.
