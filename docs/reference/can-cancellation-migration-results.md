# CAN cancellation migration results

[ADR 0025](../adr/0025-enforce-cancellation-in-a-guarded-transport.md) sets the
migration requirements. The [flash contract](flash-contracts.md#colt-cancellation-ownership)
owns implemented boundaries; the [Colt results](colt-cancellation-centralization-results.md)
retain the original pilot evidence.

## Shared test support

Task 1, 2026-10-25: direct execution tests and shared CAN runners now honor an
executor's existing `CancellationGuard` alias. The bounded selection proof takes
the actual named executor's alias and delegates its `TransportSetup`, then uses
`BindFlashAttempt` for one Write followed by one Read. A separate never-cancelled
token isolates the scripted delegate from the real token cancelled after an
accepted Write. Attempts are counted before delegation. The proof requires one
accepted Write, zero inner Reads, a remaining queued reply, `kCancelled`, and
exactly one Configure/Open/Close. The mixed-CAN template repeats that contract
independently in raw and ISO-15765 modes and checks forwarded mode transitions;
its instantiation remains for the mixed-CAN migration.

This is synthetic selection/wiring evidence, not a family's protocol execution.
No production executor, checkpoint, transport, clock, UdsClient, wire byte,
address guard, or lifecycle policy changes here. Non-opted-in direct execution
still accepts the blind inner Read and succeeds. The synthetic guarded probe
newly refuses that Read after its accepted Write; it is not a claim that a real
family's existing post-operation checkpoints are redundant.

### Verification and preservation

Source baseline: `24b0db79b3e0dd0ee86425343186a38dd0561fa2`; implementation review
base: `c28be51f615f7cfc409d46dbf13dae5f29f21f37` (intervening documentation only).
The controller's baseline flash gate passed 87/87 targets; full baseline passed
303 targets with 20 skipped. Task 1 retained all predecessor test cases and
assertions. Its focused candidate XML has 1,186 cases in 70 predecessor targets,
plus three new cases (1,189 total); no separate pre-edit case XML snapshot was
captured for this task.

- RED: `bazel test --config=release //src/backend/flash/testing:guarded_attempt_expectations_test`
  failed compilation on missing adapter/expectation definitions after target generation.
- GREEN, repeated after restoring both mutations:
  `bazel test --config=release //src/backend/flash/testing:guarded_attempt_expectations_test //src/backend/flash/ecu/... //src/backend/flash:bound_flash_attempt_test`
  passed all 71 targets, with three new adapter/runner cases passing.
- Runner bypass: delegating directly from BoundAttempt caused
  `BoundAttemptUsesSelectedGuard` to fail: one inner Read, consumed queued reply,
  and success instead of `kCancelled`.
- Adapter bypass independently caused `DirectOptInRefusesReadAfterAcceptedWrite`
  to fail on the same extra-Read/result/queued-reply assertions. The unguarded
  direct path and bound probe still passed. Both mutations were restored.
- `python3 scripts/gazelle_check.py --fix` regenerated the two managed BUILD files;
  new support libraries are explicitly test-only. Final regeneration, buildifier,
  buildifier-lint, scoped formatting, and `git diff --check` passed.

### Measured cost and qualification

`python3 scripts/nloc_loop.py measure --json` before any source edit reported
152,811 total / 68,434 production / 84,377 test NLOC. After fixture review:
153,180 total / 68,434 production / 84,746 test. Delta from the source baseline:
production **0**, test **+369**. Of the added test cost, 284 NLOC is support
(19 adapter, 257 bounded expectations, 8 shared-runner growth) and 85 is the new
regression suite. The initial implementation at `0331d650` had added 405 test
NLOC; consolidating its duplicated CAN exchange and blind transport removed
36 NLOC. The direct and bound proofs now share those fixtures while retaining
all three cases and assertions. Overall support growth remains; no family test
compression or production savings are claimed.

Review round 1 repeated the focused shared target before and after consolidation,
then bypassed runner guarding and adapter guarding independently. Only
`BoundAttemptUsesSelectedGuard` failed for the runner bypass; only
`DirectOptInRefusesReadAfterAcceptedWrite` failed for the adapter bypass. Each
observed an extra inner Read, a consumed queued reply, and success instead of
`kCancelled`. After restoring both, all three cases passed again. Gazelle removed
the regression target's now-unused direct scripted-transport dependency.

The [qualification matrix](../flash-qualification-matrix.md) and its family
checklists remain authoritative. No hardware was used, no family is newly opted
in, and hardware status and outstanding bench evidence remain unchanged. Full
repository candidate gates run once before PR delivery, separately from these
per-task focused checks.

## Subaru Hitachi M32R ECU CAN

Task 2, 2026-10-25; source baseline `0e5ac306`. The concrete executor now selects
`GuardedCanFlashTransport`; direct suite callers use the adapter. Audit covered
its executor, CanFlashUdsChannel, UdsClient and shared UDS exchange wrappers.
Production composition in `flash_workflow.cpp` binds the concrete executor;
the family suite and shared conformance helpers are its Execute callers.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission and preparation | FamilyExecutor validates before its precancellation poll; BoundAttempt keeps setup/Open/Close on the owned port. |
| OBK/session probes | Raw channel Send/Receive retains envelopes and error propagation; a cancelled token newly refuses the following blind-inner Read. |
| Identity and unlock | UDS retains four nonfatal identity queries, fatal session/security/jump/alive validation, 500 ms policy and all request checks. |
| Read | Nonfatal setup stays nonfatal; read-loop poll retained. Fatal B7 pages keep decryption/progress; pending continuation Read now refuses delegation after cancellation, without resend. Fatal stop remains fatal. |
| Erase | One raw send and receive/wait retry loop retain reply validation, 500 ms waits and exhaustion failure; guard refuses new I/O but never changes a delegated result. |
| Write and close | Write-loop poll retained, encryption and geometry unchanged. Six best-effort UDS close attempts still tolerate failures; checksum retains pending handling and fatal validation. |
| Events/results/cleanup | Existing warnings, phase updates and notices stay at their owners. No final cancellation sweep; BoundAttempt closes exactly once and preserves Execute error precedence. |

Both production loop polls remain. No protocol body, UdsClient or clock changed.
The new real-family BoundAttempt test cancels after an accepted framed B7 pending
reply; its queued positive reply remains unread, exactly one Read is delegated
for that exchange, and the result is `kCancelled`. This is a new refusal boundary,
not preserved behavior. The separate synthetic selection proof uses this actual
executor alias and proves generic runner wiring without family polls.

Verification commands used `bazel test --config=release` with
`//src/backend/flash/ecu:subaru_hitachi_m32r_can_executor_test`, then that target
plus `//src/backend/flash/testing:guarded_attempt_expectations_test`,
`//src/backend/flash:bound_flash_attempt_test`, and
`//src/backend/flash:guarded_can_flash_transport_test`.
Baseline: 15 XML cases passed. Blind pending regression RED: 2 delegated Reads
instead of 1, consumed queued positive reply. Selection RED: missing
`SubaruHitachiM32rCanExecutor::CancellationGuard`. Candidate: 17 cases passed;
all predecessor cases remain, including invalid-plan/precancel and abort cases.
Declaration mutation reproduced missing alias compilation failure. Runner bypass
failed the shared probe and actual-family selection proof on one extra inner
Read/success, and the real pending case on 2 Reads. Restored focused/shared
checks passed 4/4 targets. Gazelle regenerated both relevant BUILD files; scoped
clang-format and diff checks passed.

`python3 scripts/nloc_loop.py measure --json`: before 153180 total / 68434
production / 84746 test; after 153266 / 68437 / 84829. Delta: production **+3**,
test **+83**, including the shared blind-port fixture. This is support growth,
not savings or test compression. The narrow test-only fixture exposes accepted
`on_write`/`on_read` hooks and pre-delegation `write_attempts`/`read_attempts`;
it forwards scripted I/O with a separate never-cancelled token and returns the
scripted result unchanged.

The qualification matrix remains `experimental`; no hardware run occurred.
Bench evidence for cancellation during pending replies, physical ECU identity,
wire timing and successful read/write remains outstanding. Existing address
windows and hardware status are unchanged.

## Subaru TCU Hitachi M32R CAN

Task 3, 2026-10-25; source revision `5bebe882` (family source unchanged from
`0e5ac306`). Concrete public guard opt-in and direct-call adapter routing are
the only source behavior changes. Audit covered the executor, both UDS clients,
raw kernel channel, shared exchange wrappers and all Execute callers: local
suite helpers and KernelFreeCanWorkflow composition in `flash_workflow.cpp`.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/preparation | Validation still precedes precancellation; setup/Open/Close remain runner-owned on the original port. |
| Connect | Kernel-alive UDS probe keeps tolerant wrong/silent replies and shortcut; the guard newly refuses its blind-inner reply Read after an accepted Write. Diagnostic identity/session/security and kernel jump/recheck keep their separate IDs, validation and 50/200 ms pacing. |
| Read | Read-loop poll retained; strict page size, decryption, zero padding, progress and 1 ms post-page Sleep remain. Nonfatal stop reply behavior remains. |
| Erase | Pre-erase poll retained; raw `31 02 01` expected reply remains nonstandard, responsePending remains rejection. 500 ms delay and 200 ms receive stay; malformed/wrong/silent replies abort before any data. |
| Post-erase/write | Accepted erase can reach the existing start-write/window logs before UDS refuses the next request. No B6 write follows. Block range guard and write-loop poll retained; 200 ms frame delay, 500 ms receive and progress unchanged. |
| Close/checksum/results | Twenty-attempt close/checksum behavior and 100 ms checksum delay remain; family errors/logs retain precedence. No final sweep or changed completion rule; runner closes once and preserves the main error. |

All three executor polls are retained. `AcceptedEraseCancellationStopsBeforeData`
passed before opt-in, including the exact accepted-erase log tail (start write,
block address, window setup, cancellation warning, block-failed error), zero
B6 writes and no additional inner Write attempt. It is preservation evidence,
not a guard bypass proof. The real-family kernel-probe BoundAttempt test and the
synthetic actual-alias selection test supply that separate evidence.

Focused command: `bazel test --config=release
//src/backend/flash/ecu:subaru_tcu_hitachi_m32r_can_executor_test`. Baseline 57 XML
cases passed; baseline with additions passed 58 cases and failed only the new
kernel-probe refusal assertion (1 inner Read rather than 0, reply consumed).
Selection RED and declaration mutation both failed on missing
`SubaruTcuHitachiM32rCanExecutor::CancellationGuard`. Runner bypass failed that
family's selection and kernel-probe tests plus the shared bound probe on extra
inner Reads. Restored candidate: 60 cases, all predecessor cases retained,
including invalid-plan-before-precancel and all rejected-erase/no-data cases.
Family plus the same three shared verification targets listed for Task 2 passed
4/4, including after restoration. Gazelle regenerated ecu BUILD dependencies;
scoped formatting and diff checks passed.

`python3 scripts/nloc_loop.py measure --json`: 153266 / 68437 / 84829 before;
153344 / 68440 / 84904 after (total / production / test). Delta production
**+3**, test **+75**. No compression or savings are claimed. No hardware was
used; matrix `experimental` status remains. The linked
[family bench checklist](../checklists/subaru-tcu-hitachi-m32r-can-bench-checklist.md)
still blocks live writes pending erase-scope evidence, stock-read checksum
validation, physical MCU binding and corrected read-window qualification.
Cancellation-boundary bench evidence also remains outstanding.

## Subaru TCU CVT Hitachi M32R CAN

Task 4, 2026-10-25; source revision `9588c637`, family source still at the
`0e5ac306` baseline. The concrete alias opts in; direct suite calls use the
adapter. Audit covered the executor, both channel/UDS instances, shared UDS
wrappers, ExchangeCvtB6Chunk and callers (family/conformance suites plus
KernelFreeCanWorkflow in `flash_workflow.cpp`).

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/preparation | Family validation precedes precancellation; configuration and lifecycle remain on the runner-owned transport. |
| Connect | Raw alive probe retains 200 ms timeout/shortcut; UDS identity, sessions and seed use the separate 0x7e0 channel, jump/recheck use 0x7e1 with 0x7e9 replies. Nonfatal/fatal decisions and 2000 ms policy remain. |
| Read | Fatal dump setup and stop, decryption/zero padding/progress and read-loop poll retained. Guard covers every raw/UDS Read/Write including pending continuations. |
| Erase | Raw send/2000 ms Read and literal `31 02 01` acknowledgement remain; failure aborts before programming. Guard can newly refuse a blind-inner reply Read after accepted Write. |
| Write | Write-loop poll and eight original block windows remain. ExchangeCvtB6Chunk still accepts silence/unchecked content; guard newly refuses its reply Read when the accepted B6 Write cancels. No subsequent chunk progress occurs. |
| Close/results/cleanup | One fatal close per block and fatal checksum remain distinct from sibling retries. Existing notices/phases/logs and final-result rules stay; no final sweep. BoundAttempt closes once with existing error precedence. |

Both executor loop polls remain. The real-family BoundAttempt test reuses the
existing full-connect/erase/block script and cancels on the first accepted B6
Write. It requires `kCancelled` and zero delegated chunk-reply Reads. The
synthetic actual-alias selection proof is separate wiring evidence.

`bazel test --config=release
//src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_executor_test`: baseline
20 cases passed; new blind B6 regression failed before opt-in on 1 Read instead
of 0. Missing-alias selection RED and declaration mutation fail compilation.
Runner bypass fails the real B6 case and actual-family selection proof, plus
the shared bound probe, on extra Reads. After restoration the family has 22
passing XML cases, retaining silent chunks, unchecked replies, fatal chunk
transport errors, close exhaustion, invalid-plan/precancel and address tests.
Family plus the three Task 2 shared verification targets passed 4/4, including
the restored run. Gazelle, scoped formatting and diff checks passed.

`python3 scripts/nloc_loop.py measure --json`: 153344 / 68440 / 84904 before;
153386 / 68443 / 84943 after. Delta production **+3**, test **+39**. No savings
or compression claimed. No hardware used; the matrix remains `experimental`.
The original unreachable-legacy-path, corrected read-window and stop-polarity
port divergences remain documented in the matrix. Physical identity, erase
scope/boot-region preservation, successful read/write and the new cancellation
boundary still need bench evidence; no dedicated family checklist is linked
from this matrix row.
