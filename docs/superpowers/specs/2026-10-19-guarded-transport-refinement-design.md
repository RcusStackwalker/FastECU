# Colt cancellation centralization

Status: written design confirmed on 2026-10-19 after three interview rounds.
Implementation planning and rollout remain separate work.

## Intent and scope

Reduce repeated cancellation checks by assigning refusal of new flash I/O to a
transport decorator. Pilot only Mitsubishi Colt M32R CAN, removing its four
explicit executor token checks while retaining all existing behavior tests.
Test compression is a separately assessed follow-up. The
[pilot lessons](../../reference/guarded-transport-pilot-lessons.md) record why the
previous bundled experiments failed their size goals.

This design narrows [ADR 0025](../../adr/0025-enforce-cancellation-in-a-guarded-transport.md)
and defines a scoped migration under
[ADR 0024](../../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md).
It adds one CAN decorator, focused tests, and small runner/test integration.
It does not add K-Line or mixed-CAN support, another family, error normalization,
cancellation deferral, expected-silence policy, a transcript framework, generated
scenarios, an ECU emulator, or tooling retirement.

## Guard contract

GuardedCanFlashTransport implements the existing ICanFlashTransport port and
borrows the underlying transport. Existing executor signatures remain unchanged.

- Write and Read inspect the token supplied to that call before delegation.
  If cancellation is observed, return kCancelled without calling the inner
  transport. Otherwise forward the same bytes, timeout, and token.
- Return each delegated result unchanged, including success, an absent frame,
  present-empty bytes, and every typed error. Add no post-call token check,
  final token sweep, error remapping, or persistent stop-after-error state.
- ResetConnection, Configure, Open, Close, RestartIso15765, and RequestUnblock
  forward to the corresponding inner operation. They gain no cancellation
  gate. In particular, preserve an inner RestartIso15765 override and its own
  checkpoints rather than accidentally using the decorator's inherited default.
- Protocol validation and abort/continue decisions stay with their current
  owners. A declined confirmation remains an executor decision, even where its
  result uses kCancelled. Cancellation must not be described as ECU rejection.

"Next I/O" means refusing delegation when the guard observes cancellation at
entry. It is not an atomic guarantee against a concurrent cancellation signal.
Existing driver and clock handling of cancellation during accepted operations
remains in force. A late token change does not override a delegated result;
when no further cancellable work remains, completion may succeed. Family reply
validation and the runner's close-error precedence still apply.

## Integration and dependency direction

The existing [BoundAttempt::Run](../../../src/backend/flash/flash_executor.h)
must install a stack-local guard only around Execute, after successful Open. Colt
opts in through a nested guard type, for example
`using CancellationGuard = GuardedCanFlashTransport`. The runner detects that
dependent type and requires it to implement the executor's transport port.
Non-opted-in executors use their existing path.

The required dependency direction is Colt → guarded CAN transport →
flash_executor contracts.
The runner names only the dependent type; it does not include the concrete
guard or depend on Colt. No reciprocal includes, port split, runtime family
registry, or layer-visibility widening is needed. This arrangement is specified
from the current graph, not a claim that implementation has compiled.

Setup, preparation, configure/open, and final close continue on the owned
transport. Close still runs exactly once after successful initial Open;
an execution error wins over a close error, and a close-only error is returned.
RequestUnblock continues directly to the owned transport, without sharing a
pointer to the stack-local guard across threads.

Direct Execute callers must explicitly provide a guarded transport for the
migrated executor. Document that precondition and adapt Colt's direct tests,
using a small package-owned helper if useful. Do not construct a second guard
inside Colt or change the public execution signature.

The new guard and its contract suite belong to src/backend/flash. The runner,
Colt declaration/implementation, existing Colt tests, and a discriminating
runner-wiring case are the other C++ change surfaces. Regenerate managed BUILD
files with Gazelle and inspect the dependency direction. New production code
remains portable and exception-free across ports.

## Cancellation-boundary inventory

The inspected code baseline is 2b9613b8275a35fa485caa757a05ba628f163744.
Before implementation, verify these sites against the pinned implementation
base and record any drift in the pilot results.

| Removed Colt check | Accepted boundary/event change | Preserved obligation |
| --- | --- | --- |
| ReadFlashRange loop | The next request reports cancellation through shared exchange handling; warning/detail may change. | No next chunk reaches the inner transport; partial progress stays accurate. |
| FlashRangeMatches loop | Cancellation is observed by the next request. Final comparison may finish without another observation. | No extra comparison read; final content result retains its meaning. |
| UnlockAndErase after reply | Erase-complete events and phase transitions may precede refusal of the next request. | No subsequent write reaches the inner transport; events truthfully report the accepted erase result. |
| Execute after validation | A valid precancelled direct execution may emit connecting events before refusal. | Invalid-plan errors retain precedence; no request is delegated once cancellation is observed. |

Keep runner admission/setup checks, UdsClient's pre-request check, cancellable
Sleep, and platform read/write/unblock handling. UdsClient still has unguarded
callers. Desktop ReadSerial observes cancellation after the driver returns;
QtClock observes it during waits. These are separate responsibilities, not
redundant executor polls. Current completion already has no final token sweep.

## Required preservation and regression evidence

Retain all current Colt cases and assertions except documented changes to
cancellation wording/events and the explicit guard wiring. Preserve semantic
operator information, existing plan goldens, happy paths, address-window guards,
bootstrap/confirmation decisions, routine status and reply validation, verification,
and every family-specific abort return. Existing scripts remain the oracle;
no transcript infrastructure or broad fixture rewrite is required.

| Evidence | Must demonstrate |
| --- | --- |
| Guard contract suite | Precancelled Read/Write perform zero inner calls; allowed calls forward bytes, timeout, and the original token. |
| Delegated outcomes | Successful data, absent/present-empty replies, and typed failures pass unchanged, including when the token changes inside the delegate. |
| Forwarded operations | Lifecycle, restart override, and unblock reach the inner transport even after cancellation; no accidental gate or changed dispatch. |
| Actual Colt runner conformance | A cancellation-blind inner transport cancels after an accepted Write; the following Read is not delegated. Bypassing the runner's guard makes the test fail despite UdsClient's existing check. |
| Boundary and completion cases | The four inventory rows retain their I/O limits and truthful events; a completed final read can succeed after a late token change, while family verification failures still fail. |
| Family abort evidence | Removing a retained abort return fails a named regression that observes forbidden subsequent work, including rejection before erase. |
| Cleanup and scope | Close/error precedence and direct unblock remain intact; a non-opted-in executor retains its existing path. |

Use a minimal local cancellation-blind test double and existing scripted support;
do not expand the common fake into a scenario engine. ScriptConsumed alone does
not prove absence of unexpected work: use explicit attempted-operation evidence
or persistent failure observation where errors can be swallowed. Timing checks
must retain placement relative to I/O as well as duration.

Record named bypass/abort mutations and their failing regressions, restoring the
candidate afterwards. These targeted checks supplement existing coverage and
mutation policy; they are not claims of full equivalence or new global CI gates.
Run focused tests during implementation and the applicable full application,
portable/backend, platform, formatting, Gazelle, static-analysis, and coverage
gates before delivery, following [tooling](../../tooling.md).

## Measurement and stop conditions

The first change may grow NLOC: its goal is cancellation ownership, and it must
not claim compression. Bound that investment to the decorator, its focused suite,
and the integration described above. Additional infrastructure requires a new
scope decision rather than expanding this pilot to amortize it.

Pin the implementation base and candidate commits. On formatted trees with the
same Lizard version/exclusions, report whole-tree and changed-scope production,
test, and total NLOC using `python3 scripts/nloc_loop.py measure`. Include every
new/changed support file and its tests; report files added, changed, and removed.
Record the four deleted executor checks separately from NLOC. Do not count code
relocation or exclusion changes as savings.

For runtime, pin the affected-test target manifest: existing Colt executor and
plan, runner, and affected support suites on both sides, plus the new guard suite
on the candidate. Use the same machine, compiler, release configuration and
scheduling, one warm-up, then five uncached runs without concurrent builds.
Preserve the summed XML execution durations and compare their medians. Build and
mutation times are reported separately. More than 25% ordinary-runtime growth
requires explicit review before accepting or expanding the pilot; investigate
rather than silently changing the manifest, budget, or assertions.

Store pinned inputs, the boundary/assertion inventory, exact targets, measurements,
check results, mutation outcomes, and limitations in a package-relevant results
record under docs/reference. Preserve raw evidence with a durable artifact
location and hashes. Rebase changes that affect comparability require repeating
the affected baseline/candidate measurements. This spec records no pilot results.

## Completion and later work

The ownership change can merge for experimental Colt after its contract,
preservation evidence, measurements, and normal gates are satisfied. Qualification
status is unchanged. It does not delete tests or authorize another family.

A separate Colt compression change must map each removed assertion to a named
replacement owner, retain every family abort/continue decision and independent
wire/content obligation, and demonstrate an actual net NLOC reduction including
all its support costs. Report both its own delta and the cumulative pilot cost;
initial growth is not hidden as already-amortized infrastructure. The particular
compression approach remains a separate design decision.

Expansion beyond Colt requires review of those results and a recorded run of the
applicable [Colt bench checklist](../../checklists/colt_czt_47110032_can_bench_checklist.md).
The [qualification matrix](../../flash-qualification-matrix.md) remains the owner
of hardware status. Automated tests, replay, and mutations do not qualify hardware.
Families with deliberate cancellation deferral require a separate end-to-end
contract before their checks can be removed. No per-family rollout is automatic.

All nine interview decisions and the written design were confirmed on 2026-10-19.
The [replacement implementation plan](../plans/2026-10-19-colt-cancellation-centralization.md)
is review material pending an execution-method choice.
