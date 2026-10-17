# ADR 0023: Hill-Climb NLOC with Kind-Specific Guards

## Status

Accepted and implemented by `scripts/nloc_loop.py`, the manual-stage `nloc`
and `jscpd` prek hooks, the `nloc-loop` workflow and the `hill-climbing-nloc`
project skill. Design in
[the NLOC hill-climbing spec](../superpowers/specs/2026-10-17-nloc-hill-climbing.md).

## Context

About half of the 152,607 first-party C++ NLOC (2026-10-17) is test code,
and families such as the flash executor tests repeat the same setup across
packages. An agent can reduce this in small reviewed steps, but line count is
easy to lower by weakening tests: deleting cases, dropping assertions, or
folding distinct checks into a shared helper that no longer distinguishes
them. The existing PR `mutation` job mutates changed production lines, so it
has nothing to mutate on a test-only change, which is where most of this
work lands.

## Decision

- The metric is Lizard's NLOC over first-party C++, measured on the
  clang-formatted tree. It is reported, never ratcheted in CI.
- **Strict per PR.** Every loop PR lowers NLOC by at least 50. Abstractions
  ship with enough migrated call sites to pay for themselves.
- **Kinds are never mixed.** A loop PR is either *test-only* (production
  sources byte-identical to the base) or *production*.
- **Test-only PRs prove inclusion.** The set of killed mutants and the set of
  covered production lines must each contain the base's set, and the number
  of executed test cases must not drop.
- **Production PRs prove no regression on what changed.** No new surviving
  mutants on changed lines, no drop in line coverage of touched files, and no
  drop in executed test cases.
- These mutation guards block loop PRs only. Mutation testing remains
  advisory for all other PRs ([ADR 0022](0022-advisory-mutation-testing-with-mull.md)).

## Alternatives

- **Raw line count.** It rewards joining lines and deleting comments.
- **Net-positive setup PRs.** They let a speculative abstraction land before
  it has paid back any lines; the strict rule keeps every merged step
  measurable.
- **Mutation score or coverage percentage for test-only PRs.** A score can be
  held constant by trading one kill for another. Set inclusion cannot, and is
  well-defined only while production sources are unchanged, which is why the
  kinds are split.
- **Coverage and test count only.** They cannot detect a test that still runs
  the code but no longer checks its result.
- **A CI NLOC ratchet.** It would block legitimate feature work.

## Consequences

- Test-only loop PRs need a Mull run on both the base and the PR over the
  packages the touched tests cover. That runs in Linux CI, so the loop cannot
  fully qualify a step locally.
- Mull does not mutate `src/ui` or `src/platform`; loop PRs there rely on
  coverage inclusion and test count.
- A refactor that deliberately merges redundant test cases fails the
  test-count guard and needs a normal, non-loop PR.
