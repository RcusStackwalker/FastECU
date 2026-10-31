# Documentation index

Start with the row for the task. Read the relevant sections and follow detail
links as needed; the index is navigation, not a required reading list.

## Find the relevant context

| Task | Decisions and contracts | Additional context |
| --- | --- | --- |
| Definition import/loading/authoring | [Definition decisions](design-notes.md#definitions), [header contract](reference/definition-headers.md) | [Catalog lookup](reference/desktop-contracts.md#definition-catalog-lookup) |
| ROM opening, map edits, save/write preparation | [Calibration decisions](design-notes.md#calibration), [compatibility](reference/calibration-compatibility.md) | [Unresolved work](tech-debt.md) |
| Logger selection, samples, persistence, CSV | [Logging decisions](design-notes.md#logging), [logging contracts](reference/logging-contracts.md) | [Logger XML workflow](../resources/shared/config/README.md), [wire evidence](reference/logging-wire-evidence.md), [logging qualification](checklists/logging-composition-bench-checklist.md) |
| Connection, diagnostics, serial lifecycle, restart | [Desktop decisions](design-notes.md#desktop-composition), relevant [desktop contract section](reference/desktop-contracts.md) | [Connection](checklists/connection-bench-checklist.md) and [diagnostic](checklists/diagnostics-bench-checklist.md) checklists |
| Flash family or wire behavior | [Flash decisions](design-notes.md#flash-architecture), [sharing contracts](reference/flash-contracts.md), [family matrix](flash-qualification-matrix.md) | Follow the matrix's family checklist links; [bench CLI](checklists/bench-cli-checklist.md) has its own gate |
| Build graph, visibility, dependencies, tests | Relevant [ADR](adr/README.md), [coding conventions](coding-style.md) | [Google Mock reference](gmock-reference.md), [tooling](tooling.md) |
| Android native seam | [Remaining roadmap](modularization-plan.md) | [Portable boundary](design-notes.md#layering) |
| Choose cleanup or investigate a known defect | Relevant [technical debt](tech-debt.md) item | Verify its applicability against current source before scheduling |

## Document ownership

| Document | Owns |
| --- | --- |
| [Repository instructions](../AGENTS.md) | Essential constraints, commands, reading and Git workflow |
| [Design notes](design-notes.md) | Current decisions and concise rationale |
| [Domain language](../GLOSSARY.md) | Calibration, terminal-script, and logging domain terms |
| [Focused references](reference/) | Detailed compatibility contracts and bounded domain evidence |
| [Coding style](coding-style.md) | Writing/testing conventions |
| [Tooling](tooling.md) | Running static analysis, mutation testing, fuzzing, and the Sonar dedup loop locally and in CI |
| [ADRs](adr/README.md) | Structural decisions, historical context, and supersession |
| [Technical debt](tech-debt.md) | Unresolved defects and cleanup actions |
| [Modularization plan](modularization-plan.md) | Remaining Android milestones and exit criteria |
| [Qualification matrix](flash-qualification-matrix.md) | Family capabilities, hardware status, and evidence |
| [Checklists](checklists/) | Qualification procedures and run records |
| [Active work](#active-work) | Proposed designs and unfinished scoped work, when present |

Update a current description in place. Other documents link to its owner rather
than copying its inventory.

## Active work

The [size-reduction strategy](superpowers/specs/2026-10-18-size-reduction-strategy.md)
phases the reduction toward 100k NLOC: remote-mode deletion, tooling audit,
transport-level cancellation, separately assessed test compression, and
per-family rollout. The cancellation scope below is implemented; the other
strategy directions retain their own status.

CAN cancellation ownership is implemented through explicit executor opt-in,
including CAN EEPROM and mixed raw-CAN/ISO-15765. The
[flash contract](reference/flash-contracts.md#can-cancellation-ownership) owns
current boundaries; [CAN migration results](reference/can-cancellation-migration-results.md)
own per-task audits, measured support growth and delivery validation status.
The [pilot lessons](reference/guarded-transport-pilot-lessons.md) preserve the
two unsuccessful size-reduction attempts, and the
[Colt results](reference/colt-cancellation-centralization-results.md) retain the
original pilot's costs, preservation evidence and delivery CI.
[ADR 0025](adr/0025-enforce-cancellation-in-a-guarded-transport.md#per-executor-migration-requirements)
sets migration requirements. Separate compression assessment and a recorded
Colt bench run remain outstanding; hardware qualification is unchanged.

The [CAN cancellation migration plan](superpowers/plans/2026-10-25-can-cancellation-ownership.md)
has completed implementation. It is retained in this PR until its original
contents have been delivered in retained Git history; deletion belongs to a
later change. Final delivery gates are tracked in the migration results.

Task-specific specs and plans normally exist only while work is proposed or
unfinished; the history-preservation exception above applies before first delivery.
The [Sonar aspect qualification record](reference/sonar-aspect-qualification.md)
holds the evidence behind the Bazel-aspect scanner inputs;
[ADR 0027](adr/0027-use-native-bazel-coverage-for-sonar.md) records the native
coverage and pinned scanner of the SonarCloud job.
Find them, when present, with:

```sh
rg --files docs | rg '^docs/superpowers/(specs|plans)/'
```

Read only the documents relevant to the task. Proposals are context, not
authorization to execute; dated reports are baselines, not evidence of a current
scan. Completed artifacts are recovered from Git history below.

## Recover completed work and deleted-source citations

Completed specs/plans live only in Git history after decisions, unresolved work,
and enduring evidence are extracted. Before deletion, original contents must be
in history retained with the delivered work. Preserve untracked originals in a
separate delivered change before a deletion PR; one squash cannot retain a file
that it both introduces and removes.

To find a deleted source or completed document:

```sh
git log --all --diff-filter=D --name-only -- docs/superpowers
git log --all --diff-filter=D -- path/to/deleted-file
git show <deleting-commit>^:path/to/deleted-file
```

Legacy line citations refer to the operation source at its migration. BEEF-dialect
citations are pinned to `59f4e442`; use that revision when the citation names it.
ADRs remain because their decision lifecycle is durable reference material;
clearly marked historical context does not describe current implementation.
