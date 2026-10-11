# Spec 004 — Scan correctness (sizes, counts, totals)

Goal: reconcile every material number the scan reports — per-file
logical/allocated sizes, per-directory totals and counts, and global
footer totals — against independent reference measurements of the same
volume. No UI-restructuring work (spec 005) and no new columns or
formatting (spec 006) lands here; this spec makes the numbers right,
then proves it with logs and fixtures.

Reference: external measurements of the same volume show gaps that are
too large for display formatting: several root-level files report
logical size 0 here against GB-scale reference values; two large
directories differ by ~16 GB and ~22 GB at near-identical file counts;
global totals differ by ~134 GB (~29%) with −51k files / +5.6k folders.
This spec treats those reference values as the acceptance oracle.

## 1. In scope

1. **Size-source audit and fixes** (`src/services/scanner/`):
   per-file logical vs allocated provenance for both backends
   (native enumeration fields; volume-record `$FILE_NAME` vs `$DATA`
   resident/real-size fallback chain), including system-managed locked
   files at the volume root.
2. **Counting rules** (files vs folders, reparse points, junctions,
   symlinks, mount points, hardlinks, alternate streams, compressed /
   sparse / encrypted extents): one written rule per case, one
   implementation, one fixture test each.
3. **Access-denied accounting**: every skipped directory counted once
   with its OS code (`win32=` / `errno=`); end-of-scan summary already
   exists — this spec makes the count exact and the codes auditable.
4. **Aggregation ownership proof**: ancestor-chain propagation plus
   bulk final sweep produce exact totals; no double-count, no lost
   zero-size contributions.
5. **Footer and telemetry provenance**: footer `Total` labeled by
   definition (logical-size sum); free space from `QStorageInfo` with
   volume, unit, and timestamp; per-scan size-audit log (see 4f).
6. **Timestamp semantics decision**: directory rows carry one defined
   meaning (object mtime today) with subtree-max mtime logged during
   the audit so the follow-up choice in spec 006 is evidence-based.

## 2. Out of scope (stays deferred to 005 / 006)

- Explicit volume-root node, tree-only left pane, initial selection,
  root-file grouping, sort-order changes → spec 005.
- New columns (`% of Parent`, `Last Accessed`, `Owner`), unit
  formatting, file-row `1/0` display, summary line, chart modes,
  drive-selection pane → spec 006.
- New dependencies, DB schema/migrations, packaging/signing, CLI flags
  (CLI gains corrected numbers for free).
- Incremental rescans, USN journal, snapshot diffing (later milestones).

## 3. Architecture (must follow `module-boundaries.md`)

| Piece | Lives in | Rules |
| --- | --- | --- |
| Size-source fixes, counting rules, audit counters, timestamp provenance | `src/services/scanner/` (`MftRecord.*`, `MftFinder.*`, `DirectoryFinder.*`, `ScanEngine.*`) | Qt-free; no `#include <Q`; unit-tested everywhere with fixture record images |
| Audit log emission, footer provenance, free-space sourcing | `src/services/` (`ScanLogger.*`), `src/app/main.cpp` (wiring only) | QtCore only; no widgets |
| Number rendering unchanged in this spec | `src/models/`, `src/ui/` | No model/UI logic changes except footer label text |

Engine → bridge contracts unchanged (`onBatch` 512/250 ms,
chain-on-time-flush, bulk `onSweep`, ≤1 Hz analytics). This spec adds
log lines and counters only — no new signals.

## 4. Detailed behavior

### 4a. Size semantics (written once, enforced everywhere)

- **Logical size** = exact byte length of the default data stream
  (`EndOfFile` on native Windows enumeration; `$FILE_NAME` real size
  authoritative when nonzero on volume records, else unnamed `$DATA`
  resident length, else non-resident real size, else allocation-size
  fallback — in that order, each step logged in the audit when it fires
  on a zero-logical file).
- **Allocated size** = on-disk consumption (`AllocationSize` on native;
  resident bytes + non-resident cluster runs × cluster size on volume
  records; sparse runs occupy no clusters; compressed streams report
  compressed length with a flag).
- A failed metadata read is never a silent zero: it increments the
  access-denied/failed-read counter with its OS code and appears in
  the skip summary. Zero in the table always means "backend reported
  zero", never "we failed to read".

### 4b. Special files at the volume root

Locked system-managed files (paging, hibernation, swap, and similar)
must resolve through the same provenance chain as ordinary files. If
the volume reader cannot open them by record, the native enumerator's
values stand; if neither backend can stat them, they count as skipped
with codes — not as zero-byte rows. The audit log names each such file
with both backends' values (or the failure code) so the fix is
verifiable without re-scanning blind.

### 4c. Counting rules (normative)

- A directory counts as **one folder** in its parent; its own row
  shows subtree totals (files + folders beneath it, not including
  itself — matching the existing details-table contract).
- A file row represents **1 file / 0 folders** (spec 006 renders this;
  this spec guarantees the engine emits it).
- Reparse points (symlinks, junctions, mount points): listed once as
  encountered, never descended into, counted as files unless the record
  proves a directory target — and then still never traversed. Cyclic
  graphs cannot trap the scanner.
- Hardlinks: each directory entry counts once where enumerated
  (no cross-link deduplication in this spec); alternate data streams
  do not create rows (default stream only); compressed/sparse/
  encrypted files count normally with sizes per 4a.

### 4d. Access-denied and exclusion accounting

- Every `open()` failure increments `errorCount_` once, captures
  `lastError()` with the OS code, and feeds the existing end-of-scan
  skip dialog (capped display, exact count).
- No silent drops: a directory that cannot be opened contributes its
  own row (with zeros and the error noted) rather than vanishing from
  both counts and totals.

### 4e. Aggregation proof

- Per-worker local ancestor maps merge into shared atomics at each
  flush (existing); time-flushes emit ancestor chains; the bulk sweep
  after join is the exact final for every directory. Consumers keep
  replace semantics (never add).
- Regression test: a fixture tree with a zero-logical file, a reparse
  point, and an access-denied directory asserts exact totals and counts
  end to end through `ScanEngine` → bulk sweep.

### 4f. Size-audit log (the evidence engine)

Each scan writes, alongside the existing per-scan log, a
`size-audit` section (machine-parsable lines, not GUI text):

- Every emitted file with `logicalSize == 0`: full path (UTF-8,
  lossy-U+FFFD where needed), backend, size-source step that fired,
  `physicalSize`, OS code when applicable.
- Every directory whose totals changed between the last live chain
  report and the bulk sweep (name, delta bytes/files/folders) —
  proves convergence instead of asserting it.
- Run counters: records parsed, name-namespace wins
  (Win32/DOS/POSIX), `$DATA` source hits (resident / real-size /
  allocation-fallback), multi-extent hits, reparse-skips,
  access-denied count with code histogram.
- Timestamp provenance: per directory, object mtime AND subtree-max
  mtime (log only; display stays object mtime in this spec).
- Footer provenance: `totalLogical` definition, `QStorageInfo` volume,
  free/available bytes, unit, query timestamp.

### 4g. Acceptance oracle

Reference measurements of the same volume (provided by the operator)
are the oracle for the elevated proof scan. Targets (same hardware,
same volume, cold cache):

- Zero-logical files: none, except backends provably reporting zero
  (each exception named in the audit with its source step).
- Largest-gap directories: within 1% of reference on bytes with
  matching file/folder counts, or every remaining delta named in the
  audit with a rule reference (4c) or OS code (4d).
- Global totals: within 1% of reference bytes; file/folder counts
  reconciled rule-by-rule (no footer-only fixes).

## 5. Acceptance criteria

- [ ] Proof scan of the system volume (elevated, cold cache): audit
  section complete; every §4a–§4d rule has a log line or counter.
- [ ] Zero-logical list empty or fully sourced; previously-zero root
  files report reference-plausible sizes or coded skips.
- [ ] Directory and global totals within 1% of reference, or deltas
  individually sourced to a rule/code (no unexplained gaps).
- [ ] Fixture tests for each counting/size rule green; `unit` +
  `integration` + `ui` green; lint clean; `--smoke-test` /
  `--headless-check` exit 0; layering greps clean (no `#include <Q`
  in `src/services/scanner`).
- [ ] No model/UI structural changes in the diff except the footer
  label text (specs 005/006 own the rest).

## 6. Vertical slices (each: plan → implement → verify → commit)

- **004a audit log + counters only** (no behavior change). Tests:
  fixture scan asserts audit lines and counter shapes. Commit, then
  operator runs one elevated scan and returns the audit section.
- **004b size-source fixes** from audit evidence (fallback order,
  locked-file path, namespace wins). Tests: record-image fixtures +
  proof-scan numbers move.
- **004c counting/access rules** (reparse, hardlink/ADS, denied
  accounting, file-row 1/0 emission). Tests: fixture trees per rule.
- **004d aggregation + footer provenance** (sweep-convergence test,
  footer label, free-space sourcing). Tests: convergence fixture +
  label test. Final proof scan against the oracle.

## 7. Deliberately unchanged

Left-pane structure, selection rules, sorting, grouping, new columns,
unit formatting, chart modes, drive-selection UI, DB schema, CLI flags,
packaging. Engine batch cadence (512/250 ms), throttle policy, and
analytics accumulators are untouched except where a size-source fix
requires it (then logged per 4f).

---

### Appendix: instrumentation points (non-normative)

- `parseMftRecord`: namespace-win + `$DATA`-source counters.
- `MftFinder::findNext` / `DirectoryFinder::fillCurrent`: zero-logical
  audit hook + OS-code capture.
- `ScanEngine::workerMain` / `maybeFlush`: denied counter, chain-vs-
  sweep delta log.
- `ScanEngine::run` final: footer provenance + counter histogram.
- `ScanLogger`: `size-audit` section writer (UTF-8, lossy-safe).
