# Troubleshooting runbook

Symptoms observed on real machines, with causes and fixes.

## App exits silently / nothing happens on launch

The Qt runtime lives outside the system PATH. Launch with the MSYS2
UCRT64 `bin` directory prepended (see README quick start), or add
`C:\tools\msys64\ucrt64\bin` to the user PATH. A fresh shell exit code
`0xC0000135` (`STATUS_DLL_NOT_FOUND`) always means this.

## Linking fails: `cannot open output file ... Permission denied`

A running instance holds the binary lock. Close the app (Task Manager if
its window is unresponsive) and rebuild.

## Scan is slow but disk utilization stays low (NVMe)

Expected while the direct reader is inactive: runs are CPU-shaped, not
I/O-shaped. Check the scan telemetry `backend` field in the session log
— `NativeDirectory` without elevation is normal. Elevate to engage
`DirectMft`, then compare `nodes_per_sec` before concluding anything.

## Window goes unresponsive mid-scan

The usual cause is update volume, not scan speed: check telemetry
`batches` vs directory count and session-log error bursts (100+/sec of
`Cannot enumerate directory` is normal on system volumes — those are
access-denied skips, collected for the end-of-scan summary, not failures
of the scan itself).

## Fail-fast crash (`0xc0000409`) at scan start

Check `Application` event log, ID 1000: identical fault offset across
runs means a deterministic trigger. Known fixed causes:

- Unpaired surrogates in volume names vs throwing converters (fixed:
  infallible converter; lone units become U+FFFD).
- Sparse volume regions vs sum-sized image buffers (fixed: span sizing,
  holes zero-filled).

To diagnose a new one: note backend + progress from the last telemetry
line, elevation state, target path, and whether smaller targets survive
(isolate by subtree until the trigger directory is found).

## Elevated test `readsLiveVolumeWhenElevated` skips

It requires elevation **and** NTFS. Run the shell as administrator;
`isDirectScanAvailable` gates on both. Skips elsewhere are by design.

## Symlink test skips on Windows

Creating symlinks needs Developer Mode or privilege; the test
best-effort skips. Not a failure.

## `QSqlDatabase: duplicate connection name` warning

Fixed by re-registering instead of shadowing. If it reappears, some path
constructs a second `DatabaseManager` without going through
`ensureReady` — search for raw `addDatabase` calls.
