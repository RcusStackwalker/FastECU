# ADR 0024: Wire Transcripts and Goldens Are the Test Contract

## Status

Accepted. Supersedes [ADR 0023](0023-hill-climb-nloc-with-kind-specific-guards.md).
Implementation is phased in the
[size-reduction strategy](../superpowers/specs/2026-10-18-size-reduction-strategy.md).
The [Colt implementation](../reference/flash-contracts.md#colt-cancellation-ownership)
centralizes cancellation while retaining existing scripted tests; test
compression is a separate change. It does not require transcript infrastructure.

## Context

Half of the first-party C++ is test code, and the flash executor suites alone
hold 22,550 NLOC in 682 cases. About 193 of those cases re-prove the same
generic behavior (cancellation, timeout, disconnect, error propagation) family
by family, because every executor checks cancellation and transport failure
itself. ADR 0023 guarded NLOC reductions with executed-case counts and
killed-mutant and covered-line set inclusion. Those guards protect the current
test shape, so they reject the change that removes the duplication at its
source: enforcing cancellation once at the transport and testing it once. They
also compare whole kill sets, so a mutant that disappears with deleted
duplicate code fails the step even when nothing the ECU sees has changed.

What has to survive a refactor is what the ECU sees, not the number of test
cases that observed it.

## Decision

- **The contract is on the wire.** For every flash family, the protected
  behavior is the frames written and read, their order, timeouts and delays
  where specified, retry counts, address windows, and which failures abort.
- **Plan goldens and wire transcripts are the evidence.** Each family keeps
  plan golden tests and recorded read and write transcripts, replayed by a
  shared harness. A golden or transcript may be removed only with its family
  or operation, or replaced by a recording that a reviewed PR shows to differ
  only in non-wire behavior. This is the migration direction, not a claim that
  all families already have transcript tests. The scoped Colt ownership pilot
  retains its existing plan goldens and scripts without building a new harness.
- **Test shared policy at its owner.** Cancellation refusal is tested against
  the guarded transport
  ([ADR 0025](0025-enforce-cancellation-in-a-guarded-transport.md)); timeout,
  disconnect, and protocol interpretation remain tested at their owning layers.
  A migrated family adds a discriminating guard-conformance case. The ownership
  pilot retains all existing cases; later compression must justify each deletion
  with a named replacement owner and preserved assertions.
- **Family tests cover family branches.** A failure test exists where the
  family's own code decides something, including whether to abort after a
  shared operation fails. Shared error-propagation tests cannot replace evidence
  that no forbidden subsequent work occurs. Tests that only re-prove shared
  behavior are deletion candidates in the separate compression change.
- **Changed-line coverage stays.** Lines a PR adds or changes must be covered.
- **Mutation testing guards changed lines.** No new surviving mutants on lines
  a PR adds or changes. It stays advisory ([ADR 0022](0022-advisory-mutation-testing-with-mull.md))
  until CI time is acceptable, then becomes a merge gate for every PR.
  Kill-set inclusion and executed-case count are not guards.
- NLOC (Lizard, formatted tree, vendored code excluded) remains the reported
  size metric. It is not ratcheted.

## Alternatives

- **Keep ADR 0023's guards.** They forbid deleting redundant cases, so the
  duplication can only be compressed, never removed.
- **Coverage only.** A test can execute a line without checking its output; a
  transcript replay checks every frame.
- **Golden tests computed by production encoders.** They agree with the
  implementation by construction. Transcripts are recorded data, so they are an
  independent oracle for encrypted and checksummed bytes.

## Consequences

- `scripts/nloc_loop.py` guards, its ledger, the `nloc-loop` workflow and the
  `hill-climbing-nloc` skill implement ADR 0023 and are retired; `measure` is
  kept for reporting. Amended by [ADR 0029](0029-lower-nloc-with-sonar-nominated-right-sized-abstractions.md):
  the loop returns on SonarCloud nominations, under this ADR's evidence rules.
- Transcripts are test data, not C++, and are regenerated from a known-good
  implementation rather than edited by hand. A deliberate wire change
  re-records them in the same PR, with hardware evidence where the
  [qualification matrix](../flash-qualification-matrix.md) requires it.
- A transcript replay is not hardware qualification.
- `src/ui` and `src/platform` have no transcripts; changes there rely on
  changed-line coverage and their own behavior tests.
