# Lessons from the October 2026 size-reduction pilots

Reviewed on 2026-10-19 for the
[focused Colt cancellation design](../superpowers/specs/2026-10-19-guarded-transport-refinement-design.md).
These findings concern the pinned PR revisions below. They do not describe
merged implementation or hardware qualification.

These are reported measurements at the cited revisions, not measurements of
the current checkout. The attempts tested different propositions.

| Attempt | Proposition | Reported outcome |
| --- | --- | --- |
| [#756](https://github.com/RcusStackwalker/FastECU/tree/e04546b232a5d6100b1a943f73265f310d953757) | Colt cancellation decorator plus transcript infrastructure and test migration | +1,088 net NLOC: +72 production, +1,016 test. Four executor token checks removed; UdsClient's check retained. Bench exit gate unfinished. |
| [#768 postmortem](https://github.com/RcusStackwalker/FastECU/blob/012a408695953e91cda454817ca932a5fe8db753/docs/reference/2026-10-19-generative-protocol-test-pilot-postmortem.md) | Partial implementation of test-only scenario consolidation for two N83M families; explicitly excluded guarded transport | +837 scoped NLOC against a required reduction of 222; matched median test time 0.296 to 1.642 seconds against a 25% growth limit. Only six predecessor cases replaced; the planned consolidation was incomplete. |

## Findings

### Cancellation centralization is distinct from harness construction

In #756, transcript parsing, formatting, recording, replay, and their tests
outweighed the executor cuts. In #768, support and self-tests added 703 NLOC
and the executor suites added 134. Neither result establishes that a smaller
cancellation decorator cannot pay for itself. Neither licenses assuming that
future families will amortize unbounded infrastructure.

### Error delivery and a family's decision to abort have different owners

In #756, deleting per-site rejection tests allowed removal of executor early
returns without failing the remaining tests. A rejected unlock could be followed
by erase. [Restored per-site tests](https://github.com/RcusStackwalker/FastECU/commit/ec3b0fbbdc18c32080f392446cac19abead6595a)
protected these decisions. A shared test of error propagation cannot replace a
test that the executor performs no forbidden subsequent work.

### A wiring test must detect bypassing the guard

A token-aware inner transport or UdsClient can make cancellation pass even
without the guard. #756's [revised conformance test](https://github.com/RcusStackwalker/FastECU/commit/e5f116d0aa598f73fc05ce3bb4f861ee2ba54d28)
used a cancellation-blind inner transport and cancelled between Write and Read.
Bypassing the guard then consumed an extra read.

### A decorator alone cannot guarantee cancellation deferral

#756 [removed its unused critical-section API](https://github.com/RcusStackwalker/FastECU/commit/7ddb321ba778034cf0a9094af6976b8949aad576):
inner transports, clocks, UdsClient, and RequestUnblock could still interrupt.
Deferral requires a concrete family requirement and an end-to-end ownership
contract; suppressing one token check is insufficient.

### Preservation needs independent content and temporal assertions

#768 found gaps in address-sensitive payload checks, unexpected-operation
detection, and sleep placement relative to I/O. Its selected mutation set improved
from 14/20 to 20/20 detected mutations. That is useful targeted evidence, not
general equivalence. Persistent unexpected-operation evidence matters when
production intentionally swallows an error. Timing evidence must check placement
as well as duration. Tolerated family failures must retain their own policies.
The [results](https://github.com/RcusStackwalker/FastECU/blob/012a408695953e91cda454817ca932a5fe8db753/docs/reference/generative-protocol-test-pilot-results.md)
and [measurement evidence](https://github.com/RcusStackwalker/FastECU/blob/012a408695953e91cda454817ca932a5fe8db753/docs/reference/2026-10-19-generative-protocol-test-pilot-postmortem-evidence.json)
also show that new payload self-tests alone exceeded the entire runtime budget.
No profiling or ablation establishes a single cause for the runtime growth.

### Build failures and experimental failure are separate findings

#756 had new-code GCC warning-as-error failures in transcript support. Its
[mutation job](https://github.com/RcusStackwalker/FastECU/actions/runs/39173188503/job/117810427200)
failed during warmup before mutation; the root cause is unproven. #768's numeric
acceptance gates failed; that experimental verdict is separate from platform CI
status. Passing CI does not establish successful compression.

## Disposition

The design separates cancellation centralization from test compression, retains
family abort tests, and requires a guard-bypass regression. The narrowed
[ADR 0025](../adr/0025-enforce-cancellation-in-a-guarded-transport.md) owns the
cancellation contract; [ADR 0024](../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md)
owns test evidence and deletion policy.

The original Colt implementation plan is preserved in
[commit 4d7e67c0](https://github.com/RcusStackwalker/FastECU/blob/4d7e67c0/docs/superpowers/plans/2026-10-18-colt-guarded-transport-pilot.md).
It bundled a transcript framework, cancellation, critical sections, and test
removal. It is superseded by the focused design and leaves the checkout; it is
not an implementation plan for the replacement pilot. The old PRs remain
historical evidence, not dependencies or instructions to merge their code.
