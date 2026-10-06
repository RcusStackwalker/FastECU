# Documentation Arrangement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for native execution (recommended), or superpowers:subagent-driven-development if the user selects that method. Steps use checkbox (`- [ ]`) syntax for tracking. Execution starts after the user reviews this plan and selects the method.

**Goal:** Make current documentation discoverable with task-specific reading, distinct ownership, and recoverable Git history for completed work.

**Architecture:** `AGENTS.md` routes through a short documentation index to current decisions, focused references, debt, milestones, and qualification records. Checklists move together with their links; completed plans/specs are deleted only after their original content is preserved in delivered history.

**Tech Stack:** Markdown, Git, existing prek/lychee checks, existing Bazel queries. No application changes or new permanent tooling.

**Spec:** [Approved documentation arrangement](../specs/2026-10-06-documentation-arrangement-design.md).

## Global Constraints

- `AGENTS.md` is the sole repository agent entry point; remove `CLAUDE.md`.
- Move bench checklists and related bench notes into `docs/checklists/`.
- Completed plans and specs live only in Git history; create no archive directory.
- Aim for 150–250 lines overall in design notes; this is an editorial budget, not a CI limit.
- Preserve application behavior, architectural constraints, compatibility contracts, and hardware qualification gates.
- Preserve basenames when moving checklists. Update incoming references and outgoing relative links.
- Keep proposed Sonar work and its baseline evidence active. Preserve unrelated working-tree material.
- Work on a feature branch. Stage named paths only; never use a blanket add over the untracked documents.
- No production, BUILD, hook-configuration, or executable-tooling edits are required.

## Review Focus

- Untracked completed plans must remain recoverable after a squash merge; preservation and deletion need separate delivered changes (Tasks 1 and 5).
- Moving a checklist changes both its incoming paths and its own relative links; verify root-to-checklist and checklist-to-root/peer links (Task 2).
- Shortening design notes must retain subtle compatibility and cancellation contracts, not just architecture summaries (Task 3).
- Removing headings and milestone ledgers can strand fragment links or deleted-source citations; reroute both (Tasks 3–4).
- A physical-hardware unknown must remain unknown after correcting a catalog or test reference; compare status and gate text against the baseline (Task 4).

## File responsibilities

- Create `AGENTS.md` and `docs/README.md`; delete `CLAUDE.md`.
- Rewrite `docs/design-notes.md` as current decisions, rationale, and links.
- Create `docs/reference/definition-headers.md`, `calibration-compatibility.md`, `logging-contracts.md`, and `desktop-contracts.md`.
- Move the 15 bench checklist/notes files listed in Task 2 into `docs/checklists/`.
- Reconcile `docs/tech-debt.md`, `docs/modularization-plan.md`, and `docs/flash-qualification-matrix.md`.
- Update `docs/coding-style.md`, `docs/gmock-reference.md`, `docs/adr/README.md`, and `README.md` where ownership, guidance, or links change.
- Retire the three completed plans named in Tasks 1 and 5. This spec and this plan retire after their own work lands and history preservation is satisfied.
- Update links in active specs only if required; do not rewrite the Sonar proposal or its issue ledger.

## Task 1: Establish the preservation baseline

**Files:** The approved spec and this plan; the two currently untracked completed plans below. Record baseline hashes and the working-tree inventory outside the repository in `/private/tmp/fastecu-documentation-arrangement/`.

**Interfaces:** Produces a baseline revision, an exact untracked-file hash inventory, and a preservation change containing originals. Task 5 consumes proof that this preservation change is in delivered ancestry.

- [x] Capture `git status --short`, the current revision, and hashes of every pre-existing untracked document. Confirm the execution branch contains the approved spec and plan; use the worktree skill at execution time if isolation is needed.
- [x] Read the completed artifacts: `docs/superpowers/plans/2026-10-05-definition-header-policy.md` and `docs/superpowers/plans/2026-10-06-header-cleanup-stack.md`. Preserve their exact bytes in a scoped commit; leave the Sonar report, JSON ledger, and proposed spec unstaged.
- [x] Run the offline link hook on the two preservation files and verify the commit contains only their originals. Compare committed blobs against the baseline hashes.
- [ ] Prepare this preservation change, including the approved design/plan commits, for a separate PR. Publish when authorized and record the integrated preservation revision before any deletion delivery. A squash that both adds and removes an original does not satisfy this requirement.
- [ ] Verify the original plans, spec, and implementation plan can be retrieved with `git show <preservation-revision>:<path>`. Tasks 2–4 can proceed while the preservation PR awaits integration; Task 5 depends on its delivered ancestry.

## Task 2: Move qualification procedures with their references

**Files:** Move these basenames from `docs/` into `docs/checklists/`:

```text
bench-cli-checklist.md
cdbg-can-logging-bench-checklist.md
checksum-dialog-bench-notes.md
colt_czt_47110032_can_bench_checklist.md
connection-bench-checklist.md
denso-mc68hc16-bdm-bench-checklist.md
denso-sh705x-kline-bench-checklist.md
diagnostics-bench-checklist.md
logging-composition-bench-checklist.md
logging-engine-bench-checklist.md
platform-selection-bench-checklist.md
subaru-tcu-hitachi-m32r-can-bench-checklist.md
unisia-jecs-bench-checklist.md
unisia-jecs-m32r-bench-checklist.md
unisia-jecs-m32r-bootmode-bench-checklist.md
```

Modify their incoming callers, initially `CLAUDE.md`, `docs/design-notes.md`, `docs/tech-debt.md`, `docs/modularization-plan.md`, and `docs/flash-qualification-matrix.md`, plus any other live caller found by repository search.

**Interfaces:** Produces unchanged procedure basenames beneath `docs/checklists/` and working links. Later tasks consume these destinations.

- [x] Record the original basenames and procedure contents. Search the repository for each basename before moving it; include plain-text references outside Markdown.
- [x] Move all 15 files. Rewrite root-document links as `checklists/<basename>`, checklist-to-root links as `../<basename>`, and peer-checklist links as sibling paths; calculate paths from each caller rather than applying a global prefix replacement.
- [x] Run `prek run lychee --all-files`. Expect PASS; inspect representative matrix-to-checklist, checklist-to-design, and CLI-to-Colt links. Confirm all 15 destination files exist and no original checklist file remains at the root.
- [x] Review the move diff: only paths and relative references change in procedure bodies in this task. Gate text, run records, evidence hashes, and status are identical to the baseline.
- [x] Commit the moves and named caller updates as `docs: group bench checklists and repair references`.

## Task 3: Distill current decisions without losing contracts

**Files:** Rewrite `docs/design-notes.md`; create the four reference files named below; modify `docs/coding-style.md`, `docs/gmock-reference.md`, `docs/tech-debt.md`, `docs/flash-qualification-matrix.md`, and affected callers for moved content/anchors.

**Interfaces:** Produces concise subsystem sections in design notes and the focused contracts below. Task 4 routes its index to these exact destinations.

| Reference | Content extracted from existing design notes |
| --- | --- |
| `docs/reference/definition-headers.md` | Header drafts, text/CDATA, whitespace, numeric limits, encoding, validation, writing/registration, and bounded RomRaider/EcuFlash evidence |
| `docs/reference/calibration-compatibility.md` | Older edited-file warning, address/encoding compatibility, session/image ownership, save/open outcomes, and preflight versus correction cancellation |
| `docs/reference/logging-contracts.md` | Definition/selection/support/display ownership, stable IDs, default limits, capability asymmetry, snapshot filtering/raw assembly, and CSV/file formatting |
| `docs/reference/desktop-contracts.md` | Headings: Composition lifetime, Definition catalog lookup, Configuration session, Connection and identification, Diagnostic tools, UI channels; preserve borrower lifetimes, refresh outcomes, reentrancy, diagnostic quirks, and signal/remote-wait contracts |

- [x] Create a temporary disposition table outside the repository for every existing design-notes heading: current decision, reference, ADR/style, debt, qualification, or history-only. Verify no non-obvious compatibility requirement is discarded as history.
- [x] Extract the four focused contracts with present-tense descriptions and links to owning APIs/tests. Keep detailed defect/action status in debt and qualification evidence in checklists; references link to those owners. The desktop reference gets separate headings for its topics, not a chronological narrative.
- [x] Rewrite design notes around layering, backend outcomes/ports, flash boundaries/sharing, logging policy, definitions, calibration, and desktop composition. Each note states a decision, reason, and detail link. Remove execution narratives, obsolete QtTest recipes, and stale glob-migration advice.
- [x] Move still-applicable mutation/liveness guidance to the style/testing reference. Move deleted-source retrieval instructions to the documentation index when Task 4 creates it; keep a working pointer until then. Preserve `wrx02` as debt and Unisia unresolved knowledge in its family checklists; keep the removed JTAG rationale in its existing matrix row.
- [x] Update every live link to a removed design heading. Preserve a heading only when it still introduces a current decision; do not add empty alias headings to simulate compatibility. Keep separate notions of port-open success, failed identification/UI disconnect, and callback success.
- [x] Run `prek run lychee --all-files`, inspect `wc -l docs/design-notes.md`, and review the disposition table against the baseline. Expect links PASS, the 150–250-line editorial target, and all retained contracts accounted for.
- [x] Commit as `docs: distill current design decisions and focused contracts`.

## Task 4: Install task-specific navigation and reconcile ownership

**Files:** Create `AGENTS.md` and `docs/README.md`; delete `CLAUDE.md`; modify `docs/tech-debt.md`, `docs/modularization-plan.md`, `docs/flash-qualification-matrix.md`, `docs/checklists/logging-engine-bench-checklist.md`, `docs/adr/README.md`, and root `README.md`.

**Interfaces:** Produces the sole agent entry point, the topic index, and distinct current owners for debt, Android milestones, structural rationale, and qualification. Task 5 routes historical references through the index.

- [x] Migrate essential repository instructions to `AGENTS.md`: verification commands, portable/layer boundaries, shrinking ratchets, visibility discipline, warnings policy, package-owned tests/mocks, hardware gates, and PR workflow. Replace broad mandatory reading with a link to the task index and owning convention sections; remove `CLAUDE.md`.
- [x] Create `docs/README.md` with a short ownership table and routes for definition, calibration, logging, connection/diagnostics, flashing, build/test, and Android work. Route to relevant headings/references. Add a short Git retrieval recipe using deleting commits and their parents; retain the legacy Wave 5 revision where citations need it.
- [x] Revise technical debt to contain unresolved work and a brief goal statement, not build/test/architecture snapshots. Transfer the unimplemented backend-migration roadmap items there, dropping already-completed definition-header work after checking the implementation. Link to the active Sonar baseline/proposal without presenting it as a current scan.
- [x] Reduce the modularization plan to the remaining Android seam, current spike status, exact ABI/smoke exit criteria, and links to shared verification/qualification owners. Remove the completed milestone ledger and duplicated architecture/debt lists.
- [x] Reconcile the drift findings against `src/ui/desktop/widgets/mainwindow.cpp`, `menu_actions.cpp`, `src/ui/desktop/connection/connection_coordinator.cpp`, `src/backend/config/builtin_catalog.cpp`, and current BUILD/test owners. Correct flash-start expectations and catalog provenance without promoting hardware status. Remove absent Colt labels from current evidence; retain genuinely historical evidence as explicitly historical, rather than substituting an unrelated test.
- [x] Update the ADR index's ownership wording and affected ADR links to the relocated conventions. Check root `README.md` for retired spec paths and replace its deleted OEM K-Line citation with the maintained topic route or an explicit Git-history citation; do not invent missing protocol evidence.
- [x] Run `prek run lychee --all-files`. Query the literal labels used by current agent commands with `bazel query 'set(//:fastecu //src/backend/config:app_config_test //:clang_tidy_report_changed)'`; expect each label to resolve. Validate any additional current runnable labels individually, without building the application.
- [x] Follow the index as an agent would for definition authoring and Colt flashing. Expect direct routes to their contract and applicable qualification evidence without requiring unrelated roadmaps. Compare every matrix hardware-status/evidence field and blocking checklist prerequisite with the baseline; differences must be absent.
- [x] Commit as `docs: route agent reading and reconcile documentation ownership`.

## Task 5: Retire completed plans after preservation is delivered

**Files:** Delete `docs/superpowers/plans/2026-10-05-backend-migration.md`, `2026-10-05-definition-header-policy.md`, and `2026-10-06-header-cleanup-stack.md`; update any live callers. Retire this spec and plan in a later completion change once their work and preservation are delivered.

**Interfaces:** Consumes Task 1's integrated preservation revision and Task 4's transferred unresolved work. Produces a checkout containing active proposals and maintained knowledge, with completed plans recoverable through Git.

- [ ] Confirm the preservation revision is an ancestor of the deletion base, using `git merge-base --is-ancestor <preservation-revision> <deletion-base>`. Retrieve the original completed plans with `git show` and compare them against baseline hashes; also verify the approved design/plan are retained in that history. If the prerequisite is absent, retain the files and report this task pending.
- [ ] Confirm remaining backend roadmap actions are in technical debt and durable header rulings are in the reference. Delete only the three completed plans. Preserve the proposed Sonar spec and its baseline; leave no archive directory or stale live links to deleted paths.
- [ ] Run `prek run lychee --all-files` and search for the three deleted filenames. Expected: PASS and no live document links; any historical prose reference uses the index's retrieval route.
- [ ] Commit as `docs: retire preserved completed implementation plans`.
- [ ] After implementation review and delivery, remove this effort's spec and plan in a separate completion change based on their retained preservation ancestry. Until then they are active, with accurate status. Repair or remove active-directory index links if no tracked proposal remains after retirement.

## Final verification and handoff

- [ ] Run `git diff --check` and the offline link hook over all tracked Markdown plus any newly staged files. Use `git diff --name-only <implementation-base>` to confirm documentation-only scope.
- [ ] Search for live references to `CLAUDE.md`, original checklist paths, removed design headings, and retired plans. Distinguish working links from intentional historical citations in active planning artifacts.
- [ ] Recheck the pre-existing untracked-file inventory. Except for the explicitly preserved/retired completed plans and necessary link-only edits, original files and hashes are unchanged.
- [ ] Review against all eight spec acceptance criteria and the five Review Focus items. Record documentation sizes, verified links/labels, retained unknowns, and history retrieval evidence; do not call pending retirement complete.
- [ ] Prepare the scoped diff/PR description and report remaining integration dependencies. No application test/build cycle is needed for Markdown-only changes.

Native execution is recommended: these tasks edit shared navigation and references in sequence, and one implementer can keep the link/content disposition consistent. If selected, use `superpowers:executing-plans` and a final independent review as prescribed by that execution workflow. The alternative is subagent-driven task execution with review at each task boundary.
