# User flows

## F1 — Scan a local folder (Sprint 1 + spec 005 hierarchy)

1. Launch app (window geometry, splitters, units, target, and details
   sort restored; tree sort is always Size-descending).
2. Confirm target in the toolbar (Browse… or paste path). Elevated
   launches additionally enable the direct volume reader automatically.
3. Press Start (F5). Status shows `Scanning…`, tree streams in batches
   (names + hierarchy only, left pane).
4. Browse/select nodes mid-scan; Details tab follows selection.
5. Scan ends: the volume-root row (`C:\`) selects itself and expands;
   details list the volume's direct contents with root-level files
   grouped under `[N Files]`; the tree settles Size-descending
   (one full finish sort); status returns to `Ready` (with a skipped
   count when directories were inaccessible); a single summary dialog
   lists skipped paths if there were any. Fatal errors (bad target)
   pop up immediately.

## F0 — Headless scan (CLI, no GUI)

1. Run `UniversalStorageAnalyzer --cli-scan "<path>" [--no-log]`.
2. Progress, errors, and the final summary stream on stdout.
3. A per-scan diagnostic log is written unless `--no-log` is given.

## F2 — Drill into charts & analytics (Milestone 2, implemented on `feature/m2-analytics`)

1. With scan results in the model, switch right-side tabs: treemap
   (squarified tiles over the selected subtree's children; click
   selects in the tree, double-click descends), Bars (top-20 children),
   Pie (volume total vs free vs top children via `QStorageInfo`).
2. Open Extensions / Top Files / Users / Age panes: live during scans
   (≤1 Hz), exact at finish. Click an extension row to filter the tree
   to `\.ext$` (parents kept); click again or Esc to clear. Click a top
   file to select it in the tree (parents expand).
3. Open Options: age buckets (CSV days), top-N size, startup target
   (`last|home|ask`), export stub (disabled), throttle toggle. Bucket
   changes re-aggregate instantly from in-memory mtimes (no I/O).
4. When the disk saturates, the status bar shows `Throttled (disk
   busy)`; transitions are logged. `(unknown)` owner rows state
   backend coverage honestly.

## Deferred (Milestones 3–4)

- F3 duplicate search + hardlink dedup.
- F4 batch rename/archive/delete + cleanup wizard.
- F5 save/load index snapshots, diff growth, export PDF/Excel/CSV.
- F6 schedule unattended scans via OS schedulers.
