# User flows

## F1 — Scan a local folder (Sprint 1)

1. Launch app (window geometry + theme restored).
2. Confirm target in the toolbar (Browse… or paste path). Elevated
   launches additionally enable the direct volume reader automatically.
3. Press Start (F5). Status shows `Scanning…`, tree streams in batches.
4. Browse/select nodes mid-scan; Details tab follows selection.
5. Scan ends: status returns to `Ready` (with a skipped count when
   directories were inaccessible); a single summary dialog lists skipped
   paths if there were any. Fatal errors (bad target) pop up immediately.

## F0 — Headless scan (CLI, no GUI)

1. Run `UniversalStorageAnalyzer --cli-scan "<path>" [--no-log]`.
2. Progress, errors, and the final summary stream on stdout.
3. A per-scan diagnostic log is written unless `--no-log` is given.

## Deferred (Milestones 2–4)

- F2 drill into treemap/pie/bar charts.
- F3 duplicate search + hardlink dedup.
- F4 batch rename/archive/delete + cleanup wizard.
- F5 save/load index snapshots, diff growth, export PDF/Excel/CSV.
- F6 schedule unattended scans via OS schedulers.
