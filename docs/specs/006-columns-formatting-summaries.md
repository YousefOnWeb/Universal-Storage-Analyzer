# Spec 006 — Columns, formatting, file rows, summaries

Goal: complete the details presentation — `% of Parent`, `Last
Accessed`, and `Owner` columns; human-readable sizes everywhere the
existing unit selector reaches; defined file-row counts; directory
timestamp semantics matching the reference recency behavior; a
selected-node summary; a unified chart page; and a visible
drive-selection affordance. Numbers underneath stay exactly as spec
004 proved them; this spec changes presentation and the few pipeline
fields presentation needs.

## 1. In scope

1. **File-row counts**: file rows show Files `1`, Folders `0`
   (DisplayRole and SortRole alike); empty-vs-zero stays distinct
   everywhere else.
2. **`% of Parent` column**: per-child share of the selected
   parent's logical total, one decimal (e.g. `35.4%`); exact bytes
   stay in the Size column and in sorting.
3. **Unit formatting**: every rendered size (tree tooltips, details,
   bars, pie, treemap labels, footer, group row) honors the existing
   unit selector (`Auto`/`KB`/`MB`/`GB`, persisted as today);
   DisplayRole carries formatted strings, SortRole keeps raw integers
   (sorting never compares formatted text).
4. **Owner column**: engine `ownerId` flows through `FileRecord` into
   the model; display names resolve lazily via the existing
   `OwnerResolver` with `(unknown)` coverage honesty per backend.
5. **Last Accessed column**: backends capture access time alongside
   write time (`$STANDARD_INFORMATION` on volume records,
   directory-enumeration fields on native); unknown stays empty.
6. **Directory timestamp semantics**: directory Last Modified shows
   subtree-max recency (matching reference behavior); files show
   their own mtime. The 004 audit already logs both provenances.
7. **Selected-node summary**: a compact strip (size, allocated,
   files, folders, modified, accessed, owner) for the current
   selection; the footer keeps the global scan totals distinct.
8. **Chart page + drive selection**: one chart page with Bars/Pie
   modes (replacing the two separate tabs); a visible
   drive-selection control alongside the path field (replacing
   status-bar text).

## 2. Out of scope

- Size/counting semantics, aggregation, throttle, analytics values
  (spec 004 owns the numbers; this spec displays them).
- Exports, scheduling, shell integration, DB migrations (later
  milestones). VFS/remote targets unchanged.

## 3. Architecture (must follow `module-boundaries.md`)

| Piece | Lives in | Rules |
| --- | --- | --- |
| `lastAccessTs` capture, `ownerId` carry, subtree-max mtime in summaries | `src/services/scanner/` (`Finder.h`, backends, `ScanEngine.h`) | Qt-free; unit-tested with fixture images |
| `ownerId` projection, owner-name cache use | `src/services/` (`ScanTypes.h`, bridge) + `src/models/` (storage) | No widgets; resolution off the hot path |
| Formatted DisplayRole, `% of Parent` derivation, `1/0` file rows | `src/models/` | No FS/SQL; SortRole stays raw |
| Summary strip, chart modes, drive control, unit wiring | `src/ui/` | Main thread only |

## 4. Detailed behavior

### 4a. File rows: `1` file, `0` folders

`FileSystemModel::data` returns `1`/`0` (not empty) for file rows in
both DisplayRole and SortRole. Directories keep subtree totals.
Rationale: an empty cell reads as "uncomputed"; zero is a value.

### 4b. `% of Parent`

Computed at display time: `100.0 * child.logical / parent.logical`
(one decimal + `%`); parent total 0 → `—` (em dash, never `0.0%`
from nothing). Denominator is the SELECTED parent's current total,
so live scans show moving values that settle at the sweep — same
replace-semantics story as every other number.

### 4c. Units everywhere

A single formatter (the existing unit-aware one, promoted for reuse)
renders all size DisplayRoles, tooltips, chart labels, and footer
values from the persisted `main/units` selector. `Auto` picks
B→KB→MB→GB→TB by magnitude (1024-based, matching current footer
math); exact bytes remain one tooltip/copy away and stay the sort
keys. Tests assert strings at fixed-unit settings and raw SortRole
values.

### 4d. Owner pipeline

`DirEntry.ownerId` (already captured) → `FileRecord.ownerId` (new
`std::string`) → `Node::ownerId` → Owner column display via a
model-held `OwnerResolver` (same lazy cache semantics as the Users
pane). Empty id renders `(unknown)` with the backend-honest tooltip
the Users pane already uses. No per-file syscalls: resolution runs
once per distinct id.

### 4e. Accessed time pipeline

`DirEntry`/`EngineNode`/`FileRecord`/`Node` gain `lastAccessTs`
(Unix seconds, 0 = unknown → empty cell): volume records read
`$STANDARD_INFORMATION` access time; native backends read the
enumeration's access field (Windows `FileBasicInformation`-class
data already in hand; POSIX `lstat` atime). No extra syscall per
file beyond fields already fetched.

### 4f. Directory recency

Directory Last Modified = max(object mtime, subtree-max mtime),
computed engine-side (the `DirAcc::maxMtime` audit tracker becomes a
first-class summary field) and stored verbatim by the model. Files
keep their own mtime. Rationale recorded: reference directories
show recency (e.g. a `Users`-grade folder dated this week), and the
standing rule is to match reference behavior where it is sane.

### 4g. Summary strip and footer discipline

- Summary strip (above the details table): the SELECTED node's
  size, allocated, files, folders, modified, accessed, owner.
- Footer: the SCAN totals (files, folders, logical total, free
  space) — never the selection. The two are labeled so they cannot
  be confused.

### 4h. Chart page and drives

- One `Chart` page with a Bars/Pie mode switch; existing painters
  reused, drill-down preserved, free-space slice stays informational.
- Toolbar drive control listing mounted volumes (same source as the
  current status text); picking one sets the target path. The status
  text remains as the read-out.

## 5. Acceptance criteria

- [ ] File rows read `1` / `0`; no empty-where-zero cells in the
  table; fixture asserts both roles.
- [ ] `% of Parent` matches an independent recomputation on a
  fixture (one decimal; `—` on zero denominators).
- [ ] Fixed-unit runs render `162.1 GB`-class strings in details,
  bars, pie, treemap, footer, and group rows; SortRole stays raw;
  `Auto` buckets by magnitude.
- [ ] Owner column shows resolved names with `(unknown)` honesty
  states; resolver cache covered by unit tests (fake ids).
- [ ] Accessed column shows backend times, empty where unknown;
  no new per-file syscalls (review + fixture).
- [ ] Directory Modified reflects subtree recency on a fixture
  (child newer than parent object → parent shows child time).
- [ ] Summary strip tracks selection; footer tracks the scan; a UI
  test selects root, group, dir, and file and reads both.
- [ ] Chart modes switch without data loss; drill-down works in
  both; drive control sets the target.
- [ ] `unit` + `integration` + `ui` green; lint clean;
  `--smoke-test` / `--headless-check` exit 0; layering greps clean.

## 6. Vertical slices (each: plan → implement → verify → commit)

- **006a file-row counts + `% of Parent`** (model/display only).
  Tests: role matrix + share recomputation fixtures.
- **006b unit formatting everywhere** (DisplayRole strings, raw
  SortRole; update existing string-sensitive asserts).
- **006c owner pipeline** (record → model → resolved display).
  Tests: fake-id cache + column tests.
- **006d access-time pipeline** (backend capture + column).
  Tests: fixture images with known atimes.
- **006e directory recency** (summary-field promotion + model
  storage). Tests: newer-child fixture.
- **006f summary strip + chart page + drive control** (UI only).
  Tests: selection-tracking + mode-switch + target-setting.

## 7. Deliberately unchanged

Aggregation, batching, throttle, analytics, DB schema, CLI flags,
packaging. Tree fixed Size↓ order and group-row rules (spec 005)
are untouched — new columns participate in the existing sort
contract (raw numerics, deterministic ties).
