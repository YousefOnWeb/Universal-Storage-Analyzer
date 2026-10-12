# Spec 005 — Volume-root hierarchy, selection, and sorting

Goal: make the tree an honest navigator for the scanned volume —
explicit volume root, tree-only left pane, defined initial selection,
consistent root-file grouping, and a sort order that always applies.
Pure UI/model restructuring over the corrected numbers from spec 004;
no size/counting changes, no new columns or formatting (spec 006).

## 1. In scope

1. **Explicit volume-root node** (`FileSystemModel` + engine): the
   scanned volume itself is the depth-zero root row (e.g. `C:\` for a
   `C:/` scan). Every top-level directory and root-level file hangs
   directly beneath it. The path field keeps showing the scanned path
   as text; the hierarchy additionally represents it as a node, so
   navigation, selection, and right-pane details agree.
2. **Tree-only left pane** (`MainWindow` + `FileSystemModel`): the
   left view shows hierarchy only — names with expand/collapse,
   default-sorted by size — without the Size/Allocated/Files/Folders/
   Last-Modified columns that duplicate the details table. Metadata
   inspection belongs to the right pane.
3. **Initial selection = volume root**: after a scan finishes, the
   selection is the root node and the right pane lists the volume's
   direct contents. The scanned path (where the scan ran) and the
   selected node (what is inspected) stay distinct concepts.
4. **Root-file grouping rule**: files directly inside the volume root
   aggregate into a single synthetic group row (naming per UI copy
   review, e.g. `[N Files]` with summed size/allocation). The group
   reconciles exactly with its members (sum check in tests); members
   remain individually listable through the group. Applies at the
   volume root only — ordinary directories list member files
   individually as today.
5. **Consistent sorting** (`DetailsTableModel` + proxies): the active
   sort (column + direction, header indicator included) always
   applies to the current table; default is Size descending so the
   largest consumers top the list. Sorting compares raw integers via
   the existing numeric sort role (never display strings).

## 2. Out of scope (stays deferred to 006 / later)

- New columns (`% of Parent`, `Last Accessed`, `Owner`), unit
  formatting, file-row `1/0` display, node summary line → spec 006.
- Chart page modes, drive-selection pane → spec 006.
- Size/counting semantics, aggregation, throttle, analytics,
  DB schema, CLI flags, packaging. Engine batch cadence untouched.

## 3. Architecture (must follow `module-boundaries.md`)

| Piece | Lives in | Rules |
| --- | --- | --- |
| Root-node ownership, grouping sums, sort keys | `src/models/` (`FileSystemModel`, `DetailsTableModel`, proxies) | No FS/SQL/widgets; numeric sort role only |
| Pane restructuring, selection rules, splitter persistence | `src/ui/` (`MainWindow`) | Main thread only; queued model signals |
| Engine root-id contract (stable id 0 already exists) | `src/services/scanner/` | No UI concepts leak down; Qt-free untouched |

`EngineNode` id 0 is already the scan root; this spec projects it as
a visible row instead of an implicit parent. No engine signal changes.

## 4. Detailed behavior

### 4a. Explicit root node

- `FileSystemModel::onScanStarted` creates the visible root row from
  the target path (display name = normalized volume root, e.g.
  `C:\`); id 0 stays the stable identity for summaries, treemap, and
  analytics selection mapping.
- All previously top-level rows reparent beneath root without id
  changes (summaries, pending maps, and proxies key on ids, so live
  updates survive the move).
- Selecting the root represents the entire volume: details show its
  direct children; treemap/bars/pie scope to it; footer totals equal
  its row.

### 4b. Tree-only left pane

- Left `QTreeView` shows column 0 (name) only; the remaining model
  columns stay in the model (details/proxies/analytics read them)
  but are hidden in this view.
- Default sort: size descending (largest first). Expand/collapse
  affects visibility only; parent-child relationships never flatten.
- Accessibility: visible focus, keyboard expand/collapse/selection,
  hierarchy exposed through the accessible tree roles.

### 4c. Initial selection and path-vs-selection

- On `scanFinished`, selection moves to the root index (guarded by the
  existing task-id check; ignored for stale/cancelled scans).
- The toolbar path field (scan target) and the tree selection
  (inspected node) are labeled and stored separately; starting a new
  scan never steals the selection until its own root lands.

### 4d. Root-file group row

- Synthetic child of the root aggregating direct-member files only
  (never directories, never nested content): label with count,
  summed logical + allocated sizes, no mtime (or the max member
  mtime — decided in implementation plan from the timestamp evidence
  in the 004 audit).
- Reconciliation test: group sums == sum over member rows, updated
  by the same summary path (replace semantics, bulk sweep included).
- Clicking the group lists its member files in the details pane
  (same projection as any directory selection).

### 4e. Sorting contract

- One active sort descriptor (column, order) shared by tree and
  details; header click changes it; header indicator always reflects
  it. No second hidden sort path.
- Comparators read `SortRole` numerics; directories-vs-files tiebreak
  is deterministic and documented in the plan (proposal: directories
  and files interleave purely by the sorted column, ties by name).
- Live inserts during scans reuse the existing frozen-sort behavior
  (no per-insert re-sort, no finish re-sort — specs 002/004);
  the descriptor applies to newly visible rows without re-sorting
  the world.

## 5. Acceptance criteria

- [ ] Fresh `C:/` scan: first tree row is the volume root; its
  children are exactly the top-level entries; selecting root shows
  the volume contents on the right.
- [ ] Left pane shows names + hierarchy only; right pane carries all
  metadata; automated check asserts hidden columns in the left view.
- [ ] Group row present iff root-level files exist; sums reconcile
  exactly; members individually listable.
- [ ] Default Size-descending order verified on a fixture (largest
  first, ties deterministic); header indicator matches; switching
  columns reorders fully (no half-sorted states).
- [ ] `unit` + `integration` + `ui` green (new: root/group/sort
  tests); lint clean; `--smoke-test` / `--headless-check` exit 0;
  layering greps clean.
- [ ] No engine/scanner diff except root-id plumbing if required
  (justify per file in the implementation plan).

## 6. Vertical slices (each: plan → implement → verify → commit)

- **005a root node + initial selection** (model + selection, no pane
  changes). Tests: root row shape, selection-on-finish, stale-scan
  guard.
- **005b tree-only pane + sorting contract** (view columns, shared
  descriptor, fixture order tests).
- **005c root-file group row** (aggregation, reconciliation, member
  listing tests).
- **005d docs + state persistence** (splitter/geometry for the new
  layout, progress + user-flows/non-goals updates).

## 7. Deliberately unchanged

Sizes, counts, aggregation, throttle, analytics values, owner
resolution, telemetry, DB schema, CLI, packaging. Details columns and
their formatting stay byte-exact until spec 006.
