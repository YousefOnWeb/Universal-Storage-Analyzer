# IPC / thread contracts

Sprint 1 has no child processes; "contracts" are the cross-thread Qt
signal/slot boundaries. Worker-to-GUI traffic is queued connections;
pause/cancel deliberately use direct connections (see below).

## UI → engine (`IScannerService`)

- `startScan(targetPath, enableLogging=true) -> taskId`
- `toggleScanState(taskId, pause)`
- `cancelScan(taskId)` (cooperative; worker drains current directory entry)

Pause/cancel use direct connections and touch only the engine's
thread-safe primitives, so they take effect immediately even while a
scan blocks the worker thread (no event-loop round trip).

## Engine → UI (events)

- `scanStarted(taskId, targetPath)` — model resets root.
- `nodesDiscovered(taskId, QVector<FileRecord> batch)` — ≤512/batch,
  parent-sorted rechunk, 250 ms flush cadence.
- `branchMetricsUpdated(taskId, BranchMetrics)` — live directory
  rollups (replace semantics; best-effort during scan).
- `sweepUpdated(taskId, QVector<BranchMetrics>)` — exact finals for
  every directory as ONE bulk delivery after workers join; the model
  applies values immediately and repaints once via its coalescer.
- `analyticsUpdated(taskId, AnalyticsData)` — extensions / top files /
  age buckets / owners, absolute replace-semantics; live ≤1 Hz plus
  one exact final.
- `throttleChanged(taskId, throttled)` — disk-busy gate transitions
  only (engage after 3 bad samples, release after 10 clean).
- `scanProgress(taskId, currentPath)` — status bar (throttled 1/2000 entries).
- `scanFinished(taskId, files, dirs, bytes)` / `errorOccurred(taskId, msg, fatal)`
  (`fatal` = the scan cannot proceed, e.g. missing/non-directory target;
  per-directory skips are non-fatal and collected for an end-of-scan
  summary instead of popping up one by one).
- `noticeOccurred(taskId, msg)` — informational only (backend downgrades):
  transient status text, never collected as a skip.

Payloads are value types registered with `Q_DECLARE_METATYPE`; no pointers
cross threads. Future multi-target scans reuse `taskId` without changing shapes.
