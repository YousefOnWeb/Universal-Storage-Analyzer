# ADR 0002 — Dual-backend enumeration with queued orchestration

Date: 2026-10-06 · Status: accepted

Context: local scans must be fast on the primary filesystem yet correct
everywhere (Windows/macOS/Linux, any volume type, with or without
elevation). A single traversal implementation cannot do both.

Decision: split enumeration behind a narrow `Finder` interface
(`src/services/scanner/Finder.h`) with two backends selected per scan
root, coordinated by a `ScanEngine` over a `BlockingQueue` work pool:

- Direct volume reader: parses volume structures straight from the
  device when elevated on a supporting volume; falls back automatically
  on any failure (access, parsing, validation).
- Native directory enumerator: OS directory APIs on every platform
  (NtQueryDirectoryFile with large local / small remote buffers on
  Windows, readdir + lstat elsewhere). Always available.
- Aggregation ownership: the engine accumulates per-directory totals
  and propagates them up the ancestor chain itself (per-worker local
  maps merged into shared atomics at each flush; lock-free atomic
  `DirAcc` nodes for the tree). Consumers store absolute
  summaries verbatim and never add — this ended a latent double-count
  where per-file propagation and additive summaries overlapped.
- UI delivery: node batches (512 / 250 ms, parent-sorted rechunk) and
  per-directory metric updates over queued signals; live summaries are
  best-effort with exact finals in one bulk sweep; analytics snapshots
  ride alongside (≤1 Hz live + exact final); backends are
  invisible above the engine.

Consequences: the fast path only engages when elevated on a supported
volume; everything else behaves exactly like the portable engine with
identical output contracts. Record parsing is unit-tested with
synthetic images so it is covered on all platforms without elevation.
