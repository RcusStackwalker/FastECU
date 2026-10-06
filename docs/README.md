# Documentation index

Start with the row for the task. Read the relevant sections and follow detail
links as needed; the index is navigation, not a required reading list.

## Find the relevant context

| Task | Decisions and contracts | Additional context |
| --- | --- | --- |
| Definition import/loading/authoring | [Definition decisions](design-notes.md#definitions), [header contract](reference/definition-headers.md) | [Catalog lookup](reference/desktop-contracts.md#definition-catalog-lookup) |
| ROM opening, map edits, save/write preparation | [Calibration decisions](design-notes.md#calibration), [compatibility](reference/calibration-compatibility.md) | [Unresolved work](tech-debt.md) |
| Logger selection, samples, persistence, CSV | [Logging decisions](design-notes.md#logging), [logging contracts](reference/logging-contracts.md) | [Logging qualification](checklists/logging-composition-bench-checklist.md) |
| Connection, diagnostics, serial lifecycle, restart | [Desktop decisions](design-notes.md#desktop-composition), relevant [desktop contract section](reference/desktop-contracts.md) | [Connection](checklists/connection-bench-checklist.md) and [diagnostic](checklists/diagnostics-bench-checklist.md) checklists |
| Flash family or wire behavior | [Flash decisions](design-notes.md#flash-architecture), [family matrix](flash-qualification-matrix.md) | Follow the matrix's family checklist links; [bench CLI](checklists/bench-cli-checklist.md) has its own gate |
| Build graph, visibility, dependencies, tests | Relevant [ADR](adr/README.md), [coding conventions](coding-style.md) | [Google Mock reference](gmock-reference.md), [static analysis](coding-style.md#static-analysis) |
| Android native seam | [Remaining roadmap](modularization-plan.md) | [Portable boundary](design-notes.md#layering) |
| Choose cleanup or investigate a known defect | Relevant [technical debt](tech-debt.md) item | Verify its applicability against current source before scheduling |

## Document ownership

| Document | Owns |
| --- | --- |
| [Repository instructions](../AGENTS.md) | Essential constraints, commands, reading and Git workflow |
| [Design notes](design-notes.md) | Current decisions and concise rationale |
| [Focused references](reference/) | Detailed compatibility contracts and bounded domain evidence |
| [Coding style](coding-style.md) | Writing/testing conventions |
| [ADRs](adr/README.md) | Structural decisions, historical context, and supersession |
| [Technical debt](tech-debt.md) | Unresolved defects and cleanup actions |
| [Modularization plan](modularization-plan.md) | Remaining Android milestones and exit criteria |
| [Qualification matrix](flash-qualification-matrix.md) | Family capabilities, hardware status, and evidence |
| [Checklists](checklists/) | Qualification procedures and run records |
| [Active specs](superpowers/specs/) and [plans](superpowers/plans/) | Proposed designs and unfinished scoped work |

Update a current description in place. Other documents link to its owner rather
than copying its inventory. Active proposals are scoped context and do not
authorize execution; dated reports are baselines, not evidence of a current scan.

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

Legacy line citations refer to the operation source at its migration. Wave 5
citations are pinned to `59f4e442`; use that revision when the citation names it.
ADRs remain because their decision lifecycle is durable reference material;
clearly marked historical context does not describe current implementation.
