# ADR 0029: Lower NLOC with Sonar-Nominated Right-Sized Abstractions

## Status

Accepted. Amends the consequence of
[ADR 0024](0024-wire-transcripts-and-goldens-are-the-test-contract.md) that
retired the size-reduction loop; ADR 0024's evidence rules stand and govern
this loop. Design: the
[Sonar dedup loop spec](../superpowers/specs/2026-10-25-sonar-dedup-loop-design.md).

## Context

ADR 0024 retired the NLOC hill-climbing loop of
[ADR 0023](0023-hill-climb-nloc-with-kind-specific-guards.md) because its guards
protected test shape rather than wire behavior. The size goal remains, and
SonarCloud already measures duplication in production code: 1,621 lines on
2026-10-25, almost all of it in the flash executors. Its CPD does not analyse
test files. Driving a loop by duplication alone rewards abstractions that only
make CPD quiet, which is what
[design notes](../design-notes.md#share-only-proven-protocol-equivalence)
warn against for flash families.

## Decision

- An agent loop is restored. SonarCloud duplication groups in production code
  **nominate** where to look; first-party NLOC (Lizard, production and tests
  together) **measures** a step: each loop PR lowers it by at least 10.
- Each step adds or widens one **right-sized abstraction** (GLOSSARY), with all
  callers migrated, checked against a five-point test by a fresh review
  subagent. Family tests it makes redundant may be deleted in the same PR, each
  with a named owner of what it proved.
- A group is recorded unsuitable only when two independent reviewers find no
  right-sized abstraction; one without wire evidence is deferred. Real code is
  never added to `sonar.cpd.exclusions`.
- Loop PRs are blocked on no new surviving Mull mutants on changed lines, no
  new duplicated lines in the PR's SonarCloud analysis, changed-line coverage
  and unchanged goldens and transcripts. Mutation testing stays advisory for
  other PRs ([ADR 0022](0022-advisory-mutation-testing-with-mull.md)).
- The ledger of judgments is committed.

## Alternatives

- **Duplication as the objective.** It rewards forced merges of
  protocol-fidelity sequences and has no notion of whether the code got
  simpler.
- **jscpd over tests as a second source.** It brings back shape-driven test
  compression, which ADR 0024 replaced with contract-driven compression.
- **A duplication-density target.** Same failure as duplication as the
  objective, with a deadline.
- **Single-agent unsuitability judgments.** The judging agent is also the one
  that would have to do the work; two blind reviewers make "too hard" and
  "wrong to share" distinguishable.

## Consequences

- `nloc-loop.yml`, the `hill-climbing-nloc` skill and the ADR 0023 guards in
  `scripts/nloc_loop.py` are deleted; `measure` stays.
- Master is analysed by SonarCloud only on release; the loop triggers its own
  master analyses, provided the project's new-code period is not "previous
  analysis".
- Test-only duplication is out of the loop's reach by construction.
