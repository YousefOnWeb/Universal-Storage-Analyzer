# Non-goals (Sprint 1 + Milestone 2 shipped)

- No cloud/network/mobile/mailbox targets (local + portable fallback
  cover all current targets; remote protocols stay Milestone 3+).
- Charts and analytics (treemap, bars, pie, Extensions/Top/Users/Age
  panes with filtering and drill-down) are IMPLEMENTED on
  `feature/m2-analytics` (unreleased) — no longer placeholders.
- No dedup, mass rename, cleanup wizard, exports (Options shows a
  disabled Milestone 4 stub), or scheduling.
- No shell extensions / drag-and-drop / installer signing.

Note: direct volume reading when elevated *is* in scope and implemented
(see ADR 0002) — the portable enumerator remains the guaranteed fallback
on every platform.
