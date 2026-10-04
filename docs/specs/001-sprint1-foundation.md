# Spec 001 — Sprint 1 foundation (vertical slice)

Goal: select a local disk/directory and watch fundamental storage
consumption stream into a dual-pane UI without blocking.

## Acceptance criteria

- [ ] `File → Select Target`, Start (F5)/Stop scans a local path.
- [ ] Tree + Details update while scanning (batches ≤ 500, no UI freeze).
- [ ] Details columns: Name, Size, Allocated, Files, Folders, Last Modified;
      sortable, Quick Search filters both panes.
- [ ] System/Light/Dark theme toggle, persisted, follows OS in System mode.
- [ ] Geometry/splitter/units restored across launches (QSettings).
- [ ] `ctest -L unit` + `ctest -L integration` green; `--smoke-test` and
      `--headless-check` exit 0.
- [ ] Domain layer includes no Qt headers.

## Deliberately unchanged

Cloud/network targets, charts, dedup, mass ops, exports, scheduling,
shell integration, packaging/signing.
