# Progress

## Completed (2026-10-04)

- Project initialized per concept/discovery, architecture, Sprint 1, and
  design/contract docs: layered `src/`, `tests/`, `docs/`, `scripts/`,
  CI workflows, README/CHANGELOG.
- Sprint 1 vertical slice implemented: portable scanner worker, tree/details
  models, dual-pane main window, theming, SQLite V1 migration, CLI modes.

## Decisions

- ADR 0001: portable `std::filesystem` traversal first; MFT fast path later.
- ADR 0002: dual-backend enumeration (`Finder` + direct volume reader with
  automatic native fallback) coordinated by `ScanEngine` over a
  `BlockingQueue` pool; engine owns aggregation, model stores verbatim
  (fixes latent double-count of directory totals).
- Error severity is decided at the engine source: fatal (scan cannot
  proceed) pops up immediately; per-directory skips (108/sec observed on
  real volumes) are collected and shown once at scan end.

## Next tasks

- Milestone 2: treemap/pie/bar charts + Extensions/Users/Age/Top analytics.
- Milestone 2: VFS + SSH/S3/Azure/WebDAV read paths behind `IScannerService`.
- Harden: pause/resume button, error rows for denied paths, export stubs live.
