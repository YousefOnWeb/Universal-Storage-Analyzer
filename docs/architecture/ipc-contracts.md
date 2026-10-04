# IPC / thread contracts

Sprint 1 has no child processes; "contracts" are the cross-thread Qt
signal/slot boundaries (all `Qt::QueuedConnection` from worker to GUI).

## UI → engine (`IScannerService`)

- `startScan(targetPath) -> taskId`
- `toggleScanState(taskId, pause)`
- `cancelScan(taskId)` (cooperative; worker drains current directory entry)

## Engine → UI (events)

- `scanStarted(taskId, targetPath)` — model resets root.
- `nodesDiscovered(taskId, QVector<FileRecord> batch)` — ≤500/batch.
- `branchMetricsUpdated(taskId, BranchMetrics)` — directory rollups.
- `scanProgress(taskId, currentPath)` — status bar (throttled 1/2000 entries).
- `scanFinished(taskId, files, dirs, bytes)` / `errorOccurred(taskId, msg)`.

Payloads are value types registered with `Q_DECLARE_METATYPE`; no pointers
cross threads. Future multi-target scans reuse `taskId` without changing shapes.
