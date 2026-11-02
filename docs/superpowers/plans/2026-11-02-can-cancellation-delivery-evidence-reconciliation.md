# CAN Cancellation Delivery Evidence Reconciliation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reconcile the merged CAN cancellation stack's measured size and delivery status without rewriting historical evidence or changing hardware qualification.

**Architecture:** Update the durable ADR, flash contract, migration results, and current strategy in place. Preserve time-bounded pre-merge statements, then add one dated post-merge addendum backed by exact GitHub Actions runs; keep measured results distinct from hard maxima.

**Tech Stack:** Markdown, Git history, GitHub Actions evidence, Prek/Lychee

**Spec:** `docs/superpowers/specs/2026-11-02-can-cancellation-post-merge-cleanup-design.md`

## Global Constraints

- Deliver this documentation slice before the source-cleanup slice.
- The measured final size is exactly 66,214 production / 89,960 test NLOC, or `+3` / `+1,071` against the current-master trunk.
- The approved hard maximum remains exactly 66,215 production / 89,990 test NLOC, or `+4` / `+1,101`.
- Preserve statements that were true when pre-merge evidence was recorded; add later evidence instead of silently rewriting history.
- Do not claim hardware qualification. Colt bench work, the Hitachi TCU erase-scope question, and other recorded family gaps remain outstanding.
- Do not modify or delete unrelated untracked plans and specifications.
- Work on a feature branch and stage named paths only; never commit directly to `master`.
- This is documentation-only work: run Lychee and a scoped diff review, not an application build.

## Review Focus

- **Measured value versus ceiling:** every current statement must call 66,214 / `+3` actual and 66,215 / `+4` the maximum; Task 2 audits all occurrences.
- **Historical versus current delivery status:** pre-merge paragraphs remain dated evidence while current summaries say the stack merged; Task 3 checks both forms.
- **Superseded workflow cancellation:** the zero-duration PR #976 run is concurrency-cancelled, not a failed verification; Task 3 cites both run IDs.
- **Software versus hardware completion:** software CI and merge are complete while qualification remains unchanged; Task 3 searches both claims.
- **Workspace provenance:** only the approved spec, these plans, and named owner documents are staged; every commit step checks status first.

---

### Task 1: Preserve the approved cleanup artifacts

**Files:**
- Create: `docs/superpowers/specs/2026-11-02-can-cancellation-post-merge-cleanup-design.md`
- Create: `docs/superpowers/plans/2026-11-02-can-cancellation-delivery-evidence-reconciliation.md`
- Create: `docs/superpowers/plans/2026-11-02-can-cancellation-source-residue-cleanup.md`

**Interfaces:**
- Consumes: The approved design and its two independently executable slices.
- Produces: Delivered history containing the design and both plans before any later closeout deletion.

- [ ] **Step 1: Verify only the three approved artifacts are selected**

Run:

```bash
git status --short -- \
  docs/superpowers/specs/2026-11-02-can-cancellation-post-merge-cleanup-design.md \
  docs/superpowers/plans/2026-11-02-can-cancellation-delivery-evidence-reconciliation.md \
  docs/superpowers/plans/2026-11-02-can-cancellation-source-residue-cleanup.md
```

Expected: exactly three `??` entries. Do not stage any other untracked artifact.

- [ ] **Step 2: Verify artifact links**

Run: `prek run lychee --files docs/superpowers/specs/2026-11-02-can-cancellation-post-merge-cleanup-design.md docs/superpowers/plans/2026-11-02-can-cancellation-delivery-evidence-reconciliation.md docs/superpowers/plans/2026-11-02-can-cancellation-source-residue-cleanup.md`

Expected: `lychee` passes.

- [ ] **Step 3: Commit the approved artifacts**

```bash
git add docs/superpowers/specs/2026-11-02-can-cancellation-post-merge-cleanup-design.md docs/superpowers/plans/2026-11-02-can-cancellation-delivery-evidence-reconciliation.md docs/superpowers/plans/2026-11-02-can-cancellation-source-residue-cleanup.md
git commit -m "docs: design CAN cancellation post-merge cleanup"
```

### Task 2: Correct actual measurements at their durable owners

**Files:**
- Modify: `docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md:128-132`
- Modify: `docs/reference/flash-contracts.md:155-166`
- Test: repository-wide measurement audit

**Interfaces:**
- Consumes: The measurements table in the CAN cancellation migration results.
- Produces: Current ADR and flash-contract statements that distinguish actual results from approved maxima.

- [ ] **Step 1: Record the contradictory current statements**

Run:

```bash
rg -n -C 2 "66,21[45]|\+3|\+4" \
  docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md \
  docs/reference/flash-contracts.md \
  docs/reference/can-cancellation-migration-results.md \
  docs/README.md \
  docs/superpowers/specs/2026-10-18-size-reduction-strategy.md
```

Expected: the migration results, README, and strategy report 66,214 / `+3`; the ADR and flash contract incorrectly present 66,215 / `+4` as final actuals while also defining them as maxima.

- [ ] **Step 2: Correct the ADR measurement sentence**

Change only the final-measurement sentence to report 66,214 production / 89,960 test and `+3` / `+1,071`. Leave the earlier hard-maximum paragraph at 66,215 / 89,990 and `+4` / `+1,101`.

- [ ] **Step 3: Correct the flash-contract completion evidence**

Report 66,214 production / 89,960 test against 66,211 / 88,889, with accepted growth of `+3` / `+1,071`. State separately that this remains within the approved `+4` / `+1,101` hard maxima.

- [ ] **Step 4: Audit the result/max distinction**

Run the Step 1 command again.

Expected: every statement presenting an actual final result uses 66,214 / `+3`; 66,215 / `+4` appears only as a cap, an earlier historical measurement, or an explicitly historical quote.

- [ ] **Step 5: Commit the measurement correction**

```bash
git add docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md docs/reference/flash-contracts.md
git commit -m "docs: correct CAN cancellation final measurement"
```

### Task 3: Record post-merge delivery without rewriting history

**Files:**
- Modify: `docs/reference/can-cancellation-migration-results.md:145-245`
- Modify: `docs/superpowers/specs/2026-10-18-size-reduction-strategy.md:180-198`
- Test: delivery-status and qualification audit

**Interfaces:**
- Consumes: Successful workflow runs 41442185997, 41452289210, 41452289587, 41452291404, 41452288353, 41452292234, and 41452290990; concurrency-cancelled run 41452290045.
- Produces: A dated post-merge addendum and a current strategy that distinguish delivered software from outstanding hardware qualification.

- [ ] **Step 1: Add a dated post-merge delivery addendum**

Add `## Post-merge delivery addendum (2026-11-02)` before `## Historical evidence`. State that PRs #974, #908, and #975 through #979 merged as the delivered stack; their replacement checks passed on macOS, Ubuntu, and Windows together with the configured analysis gates. Link each successful run listed in the Interfaces block.

State that run 41452290045 on PR #976 was automatically cancelled because a higher-priority replacement was waiting, and link successful replacement run 41452291404. Do not describe the cancelled run as a failed check.

State separately that software delivery does not close the Colt bench gate, Hitachi TCU erase-scope question, or any other hardware qualification gap.

- [ ] **Step 2: Preserve the pre-merge evidence paragraphs**

Leave the existing statements that remote CI, the Linux rerun, review, readiness, and merge were outstanding in their historical sections. The new addendum supplies their later disposition.

- [ ] **Step 3: Update the current size-reduction strategy**

Replace the two claims that final post-rebase delivery or atomic delivery remain outstanding with a concise statement that the software stack and its replacement delivery checks merged successfully, linked to the post-merge addendum. Retain the statement that hardware qualification remains outstanding.

- [ ] **Step 4: Audit current and historical status**

Run:

```bash
rg -n -C 2 "remain(s)? (pending|outstanding)|before submission|merged successfully|Post-merge delivery|hardware" \
  docs/reference/can-cancellation-migration-results.md \
  docs/superpowers/specs/2026-10-18-size-reduction-strategy.md \
  docs/README.md \
  docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md
```

Expected: pending delivery language remains only in time-bounded evidence or the ADR's delivery rule; current strategy and addendum say software delivery completed; hardware gaps remain explicit.

- [ ] **Step 5: Run the documentation-only repository gate**

Run: `prek run lychee --all-files`

Expected: `lychee` passes.

- [ ] **Step 6: Review the complete documentation slice**

Run:

```bash
git diff --check HEAD~2
git diff --stat HEAD~2
git diff HEAD~2 -- docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md docs/reference/flash-contracts.md docs/reference/can-cancellation-migration-results.md docs/superpowers/specs/2026-10-18-size-reduction-strategy.md
```

Expected: no whitespace errors; only the four owner documents contain implementation edits; historical claims remain and the addendum supplies current status.

- [ ] **Step 7: Commit the delivery addendum**

```bash
git add docs/reference/can-cancellation-migration-results.md docs/superpowers/specs/2026-10-18-size-reduction-strategy.md
git commit -m "docs: record CAN cancellation stack delivery"
```
