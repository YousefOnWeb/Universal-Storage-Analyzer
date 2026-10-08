# Spec 001 — Sprint 1 foundation (vertical slice)

Goal: select a local disk/directory and watch fundamental storage
consumption stream into a dual-pane UI without blocking.

## Acceptance criteria (all met)

- [x] `File → Select Target`, Start (F5)/Stop scans a local path.
- [x] Tree + Details update while scanning (batches ≤ 500, no UI freeze).
- [x] Details columns: Name, Size, Allocated, Files, Folders, Last Modified;
      sortable (numeric sort role, no string formatting in comparisons),
      Quick Search filters both panes.
- [x] System/Light/Dark theme toggle, persisted, follows OS in System mode.
- [x] Geometry/splitter/units restored across launches (QSettings).
- [x] `ctest -L unit` + `ctest -L integration` green; `--smoke-test` and
      `--headless-check` exit 0.
- [x] Domain layer includes no Qt headers (scanner layer neither).

## Deliberately unchanged

Cloud/network targets, charts, dedup, mass ops, exports, scheduling,
shell integration, packaging/signing.
