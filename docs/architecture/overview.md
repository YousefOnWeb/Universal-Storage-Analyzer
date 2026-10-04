# Architecture overview

```text
UI (Widgets, main thread) ──signals/slots──▶ ScannerService (GUI thread)
        ▲                                            │ queued invoke
        │ batches/metrics                            ▼
FileSystemModel ◀──────────────────── LocalScannerWorker (own QThread)
        │
        ├── DetailsTableModel (selection projection) + QSortFilterProxyModel
        └── QTreeView / QTableView (virtualized, visible rows only)

Domain (Qt-free): FSNode + FileTree — owned by whoever needs pure logic/tests.
Infrastructure: QtSql SQLite store (V1 migration), QSettings for prefs.
```

Dependency direction: UI → Services → Domain. Domain never includes Qt.
Long work (disk traversal) runs on the worker thread; widgets are only
touched on the GUI thread via queued connections; cross-thread QObjects
are torn down with `deleteLater()` through the owning `QThread`.
