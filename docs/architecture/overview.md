# Architecture overview

```text
UI (Widgets, main thread) ──signals/slots──▶ ScannerService (GUI thread)
        ▲                                            │ queued invoke
        │ batches/metrics/analytics                  ▼
FileSystemModel ◀──────────────────── LocalScannerWorker (own QThread,
        │                              thin Qt bridge: engine → signals)
        ├── DetailsTableModel (selection projection) + QSortFilterProxyModel
        ├── StatsTableModel (Extensions/Top/Users/Age) + sort proxies
        ├── TreeFilterProxyModel (search + extension filter, parents kept)
        └── QTreeView / QTableView (virtualized, visible rows only)
        └── TreemapWidget / BarWidget / PieWidget + OptionsDialog (QSettings)

Scan engine (Qt-free, src/services/scanner/):
  ScanEngine ──picks per root──▶ DirectMft │ NativeDirectory (Finder)
        │  BlockingQueue<DirJob> → worker pool (≤16, one Finder each)
        │  node batches (512 / 250 ms, parent-sorted rechunk) + absolute
        │  dir summaries up the chain (per-worker local aggregation,
        │  merged at flush) + one bulk onSweep at finish + Analytics
        │  snapshots (≤1 Hz live + exact final) + throttle gate
        ▼
Domain (Qt-free): reserved for pure business rules; scan state currently
lives in ScanEngine + FileSystemModel (see data-model.md).
Infrastructure: QtSql SQLite store (V1 migration), QSettings for prefs
(age buckets, top-N, startup target, throttle toggle). DiskTelemetry
(PDH/diskstats/IOKit, 1 Hz) feeds ThrottlePolicy (3 bad engage /
10 clean release); OwnerResolver maps owner ids to display names.
```

Dependency direction: UI → Services → Domain. Domain never includes Qt.
Long work (disk traversal) runs on the worker thread; widgets are only
touched on the GUI thread via queued connections; cross-thread QObjects
are torn down with `deleteLater()` through the owning `QThread`.
