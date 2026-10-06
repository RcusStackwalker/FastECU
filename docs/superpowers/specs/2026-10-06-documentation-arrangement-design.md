# Documentation arrangement for focused agent context

Date: 2026-10-06. Status: written spec approved; implementation plan awaiting review.

## Intent and scope

Agents should find current constraints and relevant design reasoning without
loading the whole documentation corpus. Each changing fact has one
authoritative home, and completed execution records leave the checkout.

The user approved task-specific navigation, concise current design notes,
focused references, and distinct ownership for decisions, debt, milestones,
and qualification. The user also specified:

- `AGENTS.md` is the sole repository agent entry point; remove `CLAUDE.md`.
- Move bench checklists and related bench notes into `docs/checklists/`.
- Completed plans and specs live only in Git history; create no archive directory.

This is a documentation change. Preserve application behavior, architectural
constraints, compatibility contracts, and hardware qualification gates.

## Arrangement and ownership

| Location | Owns | Read when |
| --- | --- | --- |
| `AGENTS.md` | Essential constraints, common verification commands, and reading instructions | Starting a task |
| `docs/README.md` | Task-to-topic routing, document ownership, and history retrieval | Finding relevant context |
| [Design notes](../../design-notes.md) | Current decisions and concise rationale | Changing the relevant design |
| [Coding style](../../coding-style.md) | C++ writing and testing conventions | Editing the relevant kind of code |
| `docs/reference/` | Detailed contracts, compatibility knowledge, and domain evidence | Working on that topic |
| [ADRs](../../adr/README.md) | Structural decisions and their lifecycle | Reconsidering a structural choice |
| [Technical debt](../../tech-debt.md) | Unresolved defects and cleanup actions | Planning work or encountering a known defect |
| [Modularization plan](../../modularization-plan.md) | Remaining Android milestones and exit criteria | Working on the Android seam |
| [Qualification matrix](../../flash-qualification-matrix.md) | Family capabilities and qualification status | Changing a hardware-facing family |
| `docs/checklists/` | Qualification procedures, run records, and evidence | Preparing or assessing hardware qualification |
| `docs/superpowers/specs/` and `docs/superpowers/plans/` | Active proposed designs and unfinished authorized work | Working on that specific proposal |

Existing useful references, including the [Google Mock reference](../../gmock-reference.md),
can keep their filenames. Move detailed material selectively rather than
creating a reference file for every small decision.

## Entry point and reading flow

Migrate the useful repository instructions from `CLAUDE.md` into `AGENTS.md`.
Keep essential build, layering, verification, Git workflow, and hardware
constraints readily visible. Point detailed conventions at their owning
documents instead of reproducing inventories or complete rule lists.

The entry point directs agents to relevant topics through `docs/README.md`.
The index routes definition, calibration, logging, connection/diagnostics,
flashing, build/test, and Android work to specific sections or focused files.
It describes what each destination owns; it carries no copied status snapshot.

Agents read the relevant section and follow detail links when their task needs
them. Remove the blanket requirement to read the entire modularization plan,
debt roadmap, and design notes before every structural change. Active proposal
documents are discoverable from the index but are not universal prerequisites
or independent authorization to execute work.

## Current design notes and focused references

Write each note as a current decision, its reason, and links to owning APIs,
ADRs, or detailed contracts. Use present tense. Aim for 150–250 lines overall;
this is an editorial budget, not a CI limit. The brief layer overview belongs
here, with structural rationale linked to applicable ADRs.

Retain reasoning that changes how a future author should work: bounded
synchronous backend policy, caller-owned lifetimes, operator decisions outside
executors, stable identities, temporary checksum images, and evidence-based
protocol sharing. Update decisions in place as the design changes.

Remove migration-wave narration, completed defect inventories, test counts,
PR results, test-writing recipes, and repeated qualification status. Move
still-applicable conventions to the style guide or testing reference; put
unresolved defects in technical debt and qualification detail in the matrix
or applicable checklist.

Extract the detailed definition-header contract into
`docs/reference/definition-headers.md`. Preserve text/CDATA semantics,
normalization, numeric limits, partial draft versus validated identity rules,
encoding, writing/registration behavior, and the scope of reference evidence.
Design notes retain the reason for permissive reading and canonical writing.
Keep useful calibration compatibility warnings and unresolved wire knowledge
in their relevant reference or qualification document rather than deleting
them with execution history.

## Checklists and qualification

Move all existing top-level bench checklist files, including the underscore-
named Colt checklist and `bench-cli-checklist.md`, and
`checksum-dialog-bench-notes.md` into `docs/checklists/`. Preserve basenames.
The matrix stays at its existing root path and links to those procedures.

Update both incoming references and outgoing relative links after the move,
including references outside `docs/`. Do not leave compatibility copies at old
paths. Each checklist retains its run records, evidence limitations, and
blocking prerequisites. Preserve the matrix's distinction between automated
evidence and hardware qualification; no status is promoted by this change.

## Debt, milestones, and history lifecycle

Technical debt owns unresolved defects, broader policy migration, and cleanup.
The modularization plan owns remaining Android milestones and exit criteria;
replace repeated architecture/debt inventories with links and remove its
completed milestone ledger. ADRs remain available because their decision
lifecycle is durable reference material. Clearly identify superseded or
historical parts without presenting them as current instructions.

When a scoped effort lands, update its current decision, remove resolved debt,
extract enduring evidence, and delete its completed plan/spec. Unimplemented
roadmap items in a completed scoped plan move to technical debt before deletion.
The proposed Sonar design remains active; its dated triage and issue ledger
remain baseline evidence while needed by that proposal. Do not treat a dated
baseline as a current scan.

Removal requires the original contents to be recoverable from history retained
with the delivered work. Existing tracked artifacts are already in history.
For an untracked completed artifact, preserve it in a separate landed commit
before a later deletion change: adding and deleting it in one squash would
lose its contents. Leave it pending until that preservation exists. Preserve
unrelated working-tree material throughout the migration.

The index provides a short Git-history retrieval recipe for removed artifacts.
At completion, retire this spec and its implementation plan under the same rule.

## Reconcile existing drift

Recheck the findings from the documentation review against the implementation
base before changing their owning documents:

- Remove obsolete QtTest guidance; use the established GoogleTest conventions.
- Correct references to removed logging compatibility targets and UI MUT memory callers.
- Replace the claim that only presentation remains outside the backend with the actual outstanding policy migration.
- Correct the logging checklist's flash-start expectation to describe stopping logging before dispatch.
- Refresh obsolete Colt test references and distinguish catalog MCU declarations from verification on physical hardware.
- Describe connection outcomes precisely: opening the port, identifying a unit, UI disconnect, and callback success are distinct.

Code and build graphs establish implemented behavior; active documents state
intended contracts. Report and reconcile disagreement explicitly. Do not
rewrite a safety requirement merely because implementation differs, and do
not change production code as part of this documentation migration.

## Validation and acceptance

Use the existing offline Markdown link/fragment checker over every added or
changed Markdown file. Check references across the repository for removed
paths, including plain-text references the link checker cannot see. Verify
Bazel labels presented as current runnable commands against the current graph.
No new permanent documentation framework or semantic-analysis service is required.

Review the result for these acceptance criteria:

1. `AGENTS.md` is the sole repository entry point and routes task-specific reading.
2. The short index identifies one owner for each topic without copying inventories.
3. Design notes describe current decisions; detailed header policy has a focused reference.
4. Every bench checklist and related bench note is under `docs/checklists/`, and references resolve.
5. Debt, milestones, ADR lifecycle, qualification status, and procedures have distinct ownership.
6. Completed plans/specs leave the checkout only after recoverable history is established; active proposals retain explicit status.
7. Existing inconsistencies are reconciled without production changes or inferred hardware qualification.
8. An agent handling definition authoring or one flash family can find its relevant contract and evidence without reading unrelated documents.

Documentation checks and a scoped diff review are sufficient for this change;
application build/tests are required only if implementation unexpectedly changes
code, build configuration, or executable tooling.
