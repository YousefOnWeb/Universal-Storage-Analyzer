# Architecture overview

```text
UI (Widgets, main thread) ──signals/slots──▶ ScannerService (GUI thread)
        ▲                                            │ queued invoke
        │ batches/metrics                            ▼
FileSystemModel ◀──────────────────── LocalScannerWorker (own QThread,
        │                              thin Qt bridge: engine → signals)
        ├── DetailsTableModel (selection projection) + QSortFilterProxyModel
        └── QTreeView / QTableView (virtualized, visible rows only)

Scan engine (Qt-free, src/services/scanner/):
  ScanEngine ──picks per root──▶ DirectMft │ NativeDirectory (Finder)
        │  BlockingQueue<DirJob> → worker pool (≤16, one Finder each)
        │  node batches (128 / 100 ms) + absolute dir summaries up the chain
        ▼
Domain (Qt-free): reserved for pure business rules; scan state currently
lives in ScanEngine + FileSystemModel (see data-model.md).
Infrastructure: QtSql SQLite store (V1 migration), QSettings for prefs.
```

Dependency direction: UI → Services → Domain. Domain never includes Qt.
Long work (disk traversal) runs on the worker thread; widgets are only
touched on the GUI thread via queued connections; cross-thread QObjects
are torn down with `deleteLater()` through the owning `QThread`.
