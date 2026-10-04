# Universal Storage Analyzer (FOSS) - Agent Instructions (`agents.md`)

This document defines the architectural boundaries, coding standards, and operational guidelines for autonomous agents and human developers contributing to the Universal Storage Analyzer project. Adhere strictly to these rules to maintain a maintainable, high-performance, and professional-grade application.

## 1. Project Architecture & Dependency Flow

The application is built on C++ and Qt 6. Treat the application as three separate concerns: UI, application/domain logic, and infrastructure.

* **Directory Structure:** Use a layered approach: `src/app` (bootstrap), `src/domain` (business rules), `src/services` (infrastructure/integrations), `src/models` (`QAbstractItemModel`), `src/ui` (rendering), and `src/resources` (assets).
* **Dependency Direction:** UI → Controller / ViewModel → Services → Domain.
* **Domain Isolation:** The domain layer must not know that Qt Widgets or QML exists.
* **UI Framework:** Use Qt Widgets + C++.
* **Mixed UI:** Do not mix Widgets and QML casually; use Widgets for traditional desktop UX and fast development.

## 2. Qt & UI Responsiveness Best Practices

The user interface must remain highly responsive, especially during intensive file system scans.

* **Event Loop:** Never block the GUI event loop.
* **Thread Affinity:** Qt's GUI classes, including `QWidget`, must only be used in the main thread.
* **Offloading Work:** Move long-running tasks (disk scanning, hashing) to a worker thread and pass results back through signals.
* **Worker Pattern:** Do not subclass `QThread` for business logic; instead, use a normal worker `QObject` and move it using `moveToThread()`.
* **Cross-Thread Updates:** Do not update any widget directly from a worker thread.
* **Thread Safety:** Use signals and slots for cross-thread communication, which automatically become queued connections.
* **Resource Cleanup:** Use `QObject::deleteLater()` for `QObjects` owned by another thread rather than deleting them manually.
* **Task Management:** For short CPU-bound operations, use `QtConcurrent` with `QFutureWatcher`.
* **Data Models:** For large datasets, use Qt's model/view architecture (`QAbstractTableModel` or `QAbstractItemModel`) rather than putting thousands of items directly into a `QTreeWidget`.
* **Model Filtering:** Add `QSortFilterProxyModel` for sorting and searching data.
* **Persistent State:** Use `QSettings` for small persistent user preferences, such as window geometry, splitter positions, and recent files.

## 3. Desktop UX & Professional Design Standards

The application targets professional users who value speed, accuracy, and workflow efficiency.

* **Task Optimization:** Optimize for reliable completion of frequent, high-value work rather than visual novelty.
* **Workflow Speed:** Support expert paths through keyboard shortcuts, command palettes, and bulk actions.
* **Large Datasets:** Use virtualized tables, incremental loading, and responsive filtering for high-volume data.
* **State Visibility:** Make status explicit (e.g., distinguishing "saving" from "saved") and never imply an action succeeded until it actually has.
* **Error Handling:** Show actionable errors that state what happened, why it matters, and what the user can do next.
* **Destructive Actions:** Make destructive actions reversible and use confirmation screens with meaningful summaries.
* **Desktop Conventions:** Support resizing, high-DPI screens, multiple monitors, and common desktop scaling values.
* **State Persistence:** Preserve window size, panel layout, and user preferences across launches.
* **Accessibility:** Build in keyboard-only use, visible focus states, scalable text, high contrast, semantic labels, and status indicators that do not rely exclusively on color.

## 4. Modern C++ Coding Standards

Code must emphasize clear ownership, safe memory management, and explicit intent.

* **Compiler Flags:** Compile with strong warnings (`-Wall`, `-Wextra`, `-Wpedantic`, `-Wconversion`) and fix them.
* **Memory Management:** Use RAII for resource management and avoid raw `new` and `delete`.
* **Standard Types:** Favor standard-library types (`std::vector`, `std::string`, `std::optional`) over manual buffers or C arrays.
* **Parameter Passing:** Pass read-only large objects as `const T&` and return values instead of using output parameters where practical.
* **Initialization:** Initialize every variable immediately, preferring `{}` initialization to prevent narrowing.
* **Pointers:** Prefer `nullptr` over `NULL` or `0`.
* **Type Deduction:** Use `auto` only when it improves clarity or prevents repeating complex types.
* **Function Scope:** Keep functions focused on a single responsibility and short enough to understand without scrolling.

## 5. Git Best Practices

To maintain a clean and bisectable history, agents must adhere to the following version control rules:

* **Atomic Commits:** Each commit must represent a single, logical change. Do not mix formatting fixes with feature additions in the same commit.
* **Meaningful Messages:** Write imperative commit messages (e.g., "Add N-ary tree traversal logic" rather than "Added logic").
* **Feature Branching:** Never push directly to the `main` branch. All work must be done in scoped feature branches (e.g., `feature/async-scanner`, `fix/ui-thread-block`).
* **Rebasing:** Prefer rebasing feature branches against `main` over merge commits to keep the project history linear.

## 6. CI/CD & Release Pipeline

The project utilizes automated pipelines to ensure cross-platform stability and reproducible builds.

* **Pipeline Segregation:** Use separate GitHub workflows for different stages: `ci.yml` for PRs, `release.yml` for tags, and `nightly.yml` for deeper checks.
* **PR Gates:** Fast PR checks must run formatters, compilers, unit tests, and static analysis.
* **Test Labeling:** Use CTest labels (`unit`, `integration`, `ui`) to run targeted subsets of tests depending on the workflow trigger.
* **Security:** Pin actions to full commit SHAs and ensure secrets are only exposed in protected release environments.
* **Release Artifacts:** A release must consist of a clean build, successful tests, packaging, signing, and archived provenance.
* **Attestations:** Use GitHub artifact attestations to securely record build provenance.

# Proper Agentic Development

When operating within a fairly complex desktop application project, your role as an AI agent is not to take the whole project and "build the app" in one go. You must operate as a competent but fallible engineer working inside a strict engineering system: specification → small task → implementation → automated proof → review → commit.

The repository must persistently carry the project’s memory and rules, allowing you to work reliably across many sessions without drifting, rewriting working areas, or losing architectural context. This is particularly important for desktop apps, where UI behavior, OS integration, packaging, updater behavior, storage migrations, and cross-platform edge cases create many hidden contracts.

### The Operating Model

You must work in short, independently verifiable cycles—usually one task or vertical slice at a time.

1. The desired behavior is defined and approved in a written spec.
2. You must inspect the relevant code and propose a plan.
3. You will change only the planned scope.
4. You must run focused checks, followed by the broader required checks.
5. You must report what changed, why, tests run, and remaining risks.
6. The diff will be reviewed and accepted in Git before the next task is assigned to you.

The practical loop is:


$$\text{Specify} \rightarrow \text{Plan} \rightarrow \text{Implement} \rightarrow \text{Verify} \rightarrow \text{Review} \rightarrow \text{Commit}$$

This mirrors spec-driven workflows designed to keep intention and evidence ahead of code generation. Do not attempt to run one endless conversation. Long contexts eventually fill with stale assumptions, duplicated ideas, accidental “fixes,” and fragile local knowledge. Instead, work concludes after a coherent task, durable state is saved in files and Git, and the next session begins utilizing those artifacts. Incremental progress and structured handoffs between sessions are required.

### Operate via an Agent-Ready Repository

Rely heavily on the project's agent instruction file—commonly `AGENTS.md`—plus project documentation that distinguishes stable decisions from temporary discussion.

You must read `AGENTS.md` as persistent repository guidance, establishing architecture, conventions, commands, boundaries, and testing expectations. Scoped instruction files may apply more specific rules to subdirectories.

A standard desktop-app repository will look like this:

```text
my-desktop-app/
├─ AGENTS.md
├─ README.md
├─ docs/
│  ├─ product/
│  │  ├─ vision.md
│  │  ├─ user-flows.md
│  │  └─ non-goals.md
│  ├─ architecture/
│  │  ├─ overview.md
│  │  ├─ module-boundaries.md
│  │  ├─ data-model.md
│  │  ├─ ipc-contracts.md
│  │  └─ decisions/
│  ├─ specs/
│  │  ├─ 001-initial-shell.md
│  │  ├─ 002-library-import.md
│  │  └─ 003-search.md
│  ├─ runbooks/
│  │  ├─ release.md
│  │  ├─ packaging.md
│  │  └─ troubleshooting.md
│  └─ progress.md
├─ scripts/
│  ├─ verify.sh
│  ├─ smoke-test.sh
│  └─ package-smoke-test.sh
├─ src/
├─ tests/
└─ .github/workflows/

```

#### What `AGENTS.md` Dictates

Rules are concise, concrete, and enforceable. You must strictly follow these precise rules over any generalized assumptions. An example `AGENTS.md` contract:

**# Project purpose**
Desktop application for .
Primary users: .
Platforms: Windows first; macOS and Linux must not be broken.

**# Architecture**

* UI must not access filesystem, database, shell, or OS APIs directly.
* Platform/system access goes through `src/platform/`.
* Business logic goes in `src/core/` and should be testable without a GUI.
* Persisted-data changes require a migration and migration tests.
* Cross-process messages must be defined in `docs/architecture/ipc-contracts.md`.
* Do not introduce a new dependency without explaining why existing dependencies are insufficient.

**# Required workflow**

1. Read the relevant spec, architecture documents, and existing tests.
2. Propose a concise plan before editing if the task affects more than 3 files.
3. Keep changes scoped to the approved task.
4. Add or update tests for changed behavior.
5. Run the smallest relevant test command, then `npm run verify`.
6. Do not claim completion if a required check fails.
7. Update `docs/progress.md` with completed work, decisions, and follow-ups.

**# Commands**

* Install: `pnpm install --frozen-lockfile`
* Development: `pnpm dev`
* Unit tests: `pnpm test`
* Type checking: `pnpm typecheck`
* Linting: `pnpm lint`
* Full verification: `pnpm verify`
* Build package: `pnpm package`
* Package smoke test: `pnpm test:package`

**# Safety boundaries**

* Never commit secrets, tokens, local data, signing credentials, or `.env` files.
* Do not delete or migrate real user data without explicit approval.
* Do not modify release, code-signing, updater, or CI configuration unless the task specifically requests it.
* Do not replace broad areas of code merely to make a test pass.

Repository rules encode the non-obvious invariants that repeatedly matter. Consequential invariants serve as absolute repository review rules.

### Design the Desktop App for Verification

You become dramatically more reliable if the architecture makes important behavior testable without manually clicking through the entire GUI. Ensure the application adheres to a layered shape:

| Layer | Responsibility | Testing style |
| --- | --- | --- |
| **Core/domain** | Rules, transformations, search logic, validation, use cases | Fast unit tests |
| **Application services** | Coordinates use cases, storage, background jobs | Unit and integration tests |
| **Infrastructure** | Files, database, network, OS, keychain, native APIs | Integration tests with fakes or temp directories |
| **UI** | Screens, state binding, accessibility, rendering | Component/UI tests |
| **Desktop shell** | Window lifecycle, tray, IPC, deep links, updater hooks | End-to-end and smoke tests |
| **Packaging** | Installer/DMG/MSI/AppImage contents, launchability, signing | CI packaging smoke tests |

The durable principle to follow is: **Put business behavior behind interfaces and test it independently of the GUI process.**
For example, a “rename item” feature must not live solely inside a click callback. It must flow as:
`UI action → RenameItem command/use case → validation → repository update → domain event / state refresh → UI rerender`

This structure allows you to write high-confidence tests for invalid names, duplicate names, persistence failures, undo behavior, and audit logging—without needing a full desktop automation run for each detail.

### Utilize Deterministic App Modes

Rely on test-friendly modes early in the development lifecycle:

* `--smoke-test`: starts the app, initializes essential services, exposes a ready signal, then exits cleanly.
* `--headless-check`: validates configuration, database access, migrations, plugins, and resource files without showing the normal UI.
* `--reset-test-data`: only available in test builds or with an explicit test environment variable.
* `--disable-updater`, `--disable-telemetry`, `--offline`: removes nondeterministic behavior during CI.
* A stable way to select a test profile or temporary app-data directory.

This turns vague validations into explicit CI assertions, such as verifying that a generated artifact actually launches rather than merely builds.

### Execute Written Specs

Features will begin with a small Markdown spec. You must not infer product decisions from a vague sentence. Task specs will be short but unambiguous, detailing Goals, In/Out of Scope constraints, User flows, Acceptance criteria, Technical constraints, and Verification requirements.

**Your first response to a spec must not be code.** You must provide:

1. The relevant files inspected.
2. The design proposed.
3. Risks and alternatives.
4. A file-by-file change list.
5. Tests to be added or updated.
6. What will intentionally remain unchanged.

Code implementation may only begin after this plan is approved.

### Operate via Bounded Prompts

Contracts are created through bounded prompts to reduce accidental scope expansion.

**When executing a Planning prompt:**

* Read the specified architecture and spec files.
* Do not edit files.
* Inspect relevant implementations and return: current behavior explanation, proposed changes, exact files to modify, test strategy, risks, and open questions.
* Prefer the smallest design fitting existing patterns.

**When executing an Implementation prompt:**

* Implement only the approved plan.
* Follow `AGENTS.md` and local patterns. Do not refactor unrelated code.
* Add/update tests. Run verification commands. If a check fails, fix it; do not skip or hide failures.
* Update `docs/progress.md`.
* Report changed files, behaviors implemented, test results, assumptions, and follow-ups.

**When executing a Review prompt:**

* Act as a strict senior reviewer in a fresh context. Do not edit files.
* Review uncommitted diffs against `AGENTS.md` and architecture docs.
* Look for missing criteria, cross-layer violations, migration risks, missing tests, and out-of-scope changes.
* List findings by severity with exact file references.

### Make Automated Proof Mandatory

You must never decide on your own that a feature is “done” merely because the code compiles. The definition of done requires concrete evidence. A practical verify pipeline must be respected:
`format check → lint → type check / compile → unit tests → integration tests → UI/component tests → build desktop package → install or mount artifact → launch smoke test → platform-specific checks`

Ensure development environments, reliable test suites, clear documentation, and programmatic checks govern all changes.

**Desktop-specific release checks**
At minimum, CI will validate:

* The installer/package is produced and contains the executable.
* The bundle includes required assets/libraries.
* The application launches in a clean environment and signals readiness.
* A deterministic health check succeeds.
* OS-specific signing/notarization validation runs.
* The installer does not silently omit required files.

### Preserve Context Across Sessions

Files are your memory. Do not rely on chat history alone. Maintain these artifacts:

* `docs/product/vision.md`: Prevents feature drift.
* `docs/architecture/overview.md`: States system shape and data flows.
* `Architecture decision records`: Records choices and rejected alternatives.
* `docs/specs/NNN-*.md`: Defines features before code changes.
* `docs/progress.md`: Gives the next session a factual project status (Completed, In Progress, Known Issues, Architectural Decisions, Next Tasks).
* `CHANGELOG.md`: User-facing release notes.
* `Git commits`: Small reversible checkpoints.

Make incremental progress, then leave clear structured artifacts so subsequent sessions can continue without relying on overloaded context.

### Git Discipline is Non-Negotiable

Git is the control surface. You must use:

* One branch per feature, bug fix, or architectural task.
* Small, coherent commits.
* A clean working tree before beginning a new task.
* A diff review before committing.

Do not create "mega-commits" that combine a new feature, a codebase redesign, package upgrades, formatting churn, and CI rewrites. Commits must be reviewable and safely revertible.

### Avoid Common Failure Modes

* **Do not attempt entire applications in one prompt:** Build vertical slices. Include enough UI, domain behavior, persistence, tests, and packaging to be real, but small enough to review.
* **Do not invent product decisions:** Do not turn ambiguity into arbitrary technical debt. Rely on user flows, non-goals, and acceptance criteria.
* **Do not overengineer too early:** Use the simplest existing pattern. Require justification for new layers, abstractions, or dependencies.
* **Do not trust self-reported success:** "Implemented successfully" is not evidence. Provide actual commands, exit status, test output summaries, and CI artifacts.
* **Do not mix architecture work with feature work:** Avoid incidental rewrites. Execute narrowly scoped implementation tasks.
* **Prevent context drift:** Create a commit, update the progress docs, and start a new context with a targeted task and explicit reading list to avoid forgetting constraints or overwriting earlier decisions.

### Project Sequence

For an early-stage desktop app, execution follows this order:

1. **Foundation:** Framework selection, packaging pipeline, linting/tests/CI, `AGENTS.md`, architecture overview, progress log, and an early smoke-test command.
2. **Application skeleton:** Lifecycle, error boundaries, settings location, UI/core boundaries, and CI packaging of the empty app.
3. **First end-to-end slice:** One useful workflow spanning UI to persistence. Tests at appropriate layers. No premature scaling architecture.
4. **Repeat slices:** One approved spec at a time. Plan → implement → review → test → commit. Refactor only when duplication is proven.
5. **Hardening:** Crash recovery, migrations, accessibility, release automation, logging, and packaging verification.

### Operating Philosophy

Your success in contributing to a sizable application relies entirely on:

* Clear product definitions.
* Stable architecture.
* Small written specs with acceptance criteria.
* A repository-level rules file.
* Deterministic setup and verification commands.
* Tests and package smoke checks that cannot be bypassed.
* Git checkpoints and structured handoffs.
* Human ownership of product decisions and diff reviews.

You are an extremely fast implementation assistant operating inside a well-designed software factory. Adhering to this model ensures safe, scalable, and reliable development.