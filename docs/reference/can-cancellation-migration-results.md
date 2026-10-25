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

## Subaru TCU CVT Mitsubishi MH8111 CAN

Task 5, 2026-10-25; source revision `f2f9caa5`, family source unchanged from
`0e5ac306`. Header alias/include and direct-suite adapter routing opt in this
family. Audit covered the executor, shared CVT B6 exchange and crypto, UDS
wrappers, raw channel paths and callers: family/conformance suites and the
KernelFreeCanWorkflow binding in `flash_workflow.cpp`.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/preparation | Existing validation/precancellation order and original-port lifecycle remain. |
| Connect | All seven exchanges stay on 0x7e1/0x7e9, unlike Hitachi's separate diagnostic channel. Nonfatal identity/session, fatal seed/key/jump and raw alive response keep their decisions and 2000 ms timeouts. |
| Read | Raw setup's unusual SID/reply pairing retained; read-loop poll/decryption/0xFF padding/progress unchanged. Raw stop permits six sends, accepts any content and succeeds after silence exhaustion; transport errors still propagate. |
| Erase | Twenty UDS erase attempts retain success matching and cancellation propagation while other failures retry. Guard changes only entry refusal, including pending continuation Reads. |
| Write | Write-loop poll retained; upper block 0x80000/0x100000 geometry, halved setup length and 256-byte chunks remain. Shared B6 Read remains content-blind with 2000 ms timeout; guard newly refuses it after cancellation from accepted Write. |
| Close/checksum/events | Close checks the standard positive SID, tolerates unchecked trailing content and retries twenty times; checksum's twenty attempts and fatal exhaustion stay. Existing logging/progress and no-final-sweep semantics remain. Runner closes once and retains error precedence. |

Both loop polls remain. Real-family BoundAttempt proof reuses the existing
connect/erase/full-block script, cancels at first accepted B6 Write and checks
zero inner chunk-reply Reads with `kCancelled`; this is a new refusal boundary.
The separate selection test uses the actual alias with the bounded probe.

`bazel test --config=release
//src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8111_can_executor_test`: 20 baseline
cases passed. Blind B6 regression RED observed 1 Read instead of 0. Selection
RED and declaration mutation fail compilation for missing alias. Runner bypass
fails real B6/selection cases and shared bound probe on extra Reads. Restored
candidate: 22 XML cases pass; all predecessor cases retained, including silent
and uninspected chunk replies, fatal transport errors, close exhaustion,
checksum, addressed upper-block writes and invalid-plan/precancel cases. Family
plus Task 2's three shared verification targets passed 4/4, including restored
run. Gazelle dependency regeneration, formatting and diff checks passed.

`python3 scripts/nloc_loop.py measure --json`: 153386 / 68443 / 84943 before;
153429 / 68446 / 84983 after. Delta production **+3**, test **+40**. No savings
or compression claimed. Matrix `experimental` status is unchanged; no hardware
used. Its nonoverlapping read/write windows, halved declared setup length and
ported erase-retry correction remain qualification gaps. Physical identity,
boot-region preservation, write/readback from the programmed upper block and
cancellation-boundary bench evidence remain outstanding; the matrix row links
no dedicated family checklist.

## Subaru TCU CVT Mitsubishi MH8104 CAN

Task 6, 2026-10-25; source revision `0e582641`, family source unchanged from
`0e5ac306`. The concrete header selects the guard and suite direct calls use
the adapter. Audit covered the local raw exchange/retry helpers, CAN envelope
channel, shared pure CVT crypto and callers (family/conformance suites and
KernelFreeCanWorkflow in `flash_workflow.cpp`). This family does not use
UdsClient or the shared CVT B6 exchange.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/preparation | Validation-before-precancel order and runner-owned setup/Open/Close remain. |
| Connect | Raw alive shortcut, six-attempt identity queries and logged-but-tolerated session/security/jump content remain. Local Exchange now reaches the guard for every Send/Read; malformed envelope and transport failures still propagate. |
| Read | Content-blind setup and B7 reply handling, read-loop poll, decrypt/0xFF padding/progress and six-attempt tolerant stop remain. Guard may refuse the raw reply Read after an accepted Write cancels. |
| Erase | Single raw send, 8000 ms delay, 200 ms Read, then 5000 ms delay and tolerated wrong-content reply remain. Clocks retain cancellation ownership at both waits. |
| Write | Original 0x8000/0x78000 addressed window and 128-byte chunks remain; write-loop poll retained. First B6 Write is accepted, then cancellation newly refuses the blind-inner reply Read at guard entry. This local receive stays 500 ms, distinct from the shared CVT exchange's 2000 ms. |
| Close/checksum/results | Six-attempt content-blind setup/close/checksum and tolerated exhaustion remain, including the 100 ms close/checksum delay. Existing errors/logs/progress stay; no final sweep or post-call remapping. Runner closes once with existing error precedence. |

Both loop polls remain. Real-family BoundAttempt proof scripts the existing
full connect/erase helpers, family-specific window and exact first encrypted B6
chunk, then cancels from its accepted Write. It requires `kCancelled`, zero
inner chunk-reply Reads and the queued acknowledgement still unconsumed. The
synthetic actual-alias probe separately proves guard selection/wiring.

`bazel test --config=release
//src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8104_can_executor_test`: 17 baseline
XML cases passed. New real B6 guard regression failed before opt-in on 1 Read
instead of 0 and consumed acknowledgement. Selection RED and declaration
mutation fail compilation for missing alias. Runner bypass fails the real B6
and selection cases plus shared bound probe on extra Reads. Restored family:
19 cases pass, retaining malformed-but-tolerated replies, addressed writes,
transport failures, invalid plans and precancellation. Family plus Task 2's
three shared verification targets passed 4/4, including after restoration.
Gazelle, scoped formatting, commit hooks and diff checks passed.

`python3 scripts/nloc_loop.py measure --json`: 153429 / 68446 / 84983 before;
153474 / 68449 / 85025 after. Delta production **+3**, test **+42**. No savings
or compression claimed. Matrix `experimental` status remains unchanged; no
hardware used. Tolerant content handling and kernel-alive recheck quirks remain
qualification concerns, not reasons to change protocol policy here. Physical
identity, erase scope, read/write/readback, timing and cancellation-boundary
bench evidence remain outstanding; no dedicated family checklist is linked
from this matrix row.

## Tasks 2–6 aggregate

The five executor `.cpp` files, all eleven explicit executor polls, UdsClient,
clocks and protocol helpers are unchanged. All direct Execute calls in these
five suites use the selected-guard adapter. The production diff is five public
aliases/includes; generated dependencies preserve existing visibility layers.
All 129 predecessor family XML cases remain; candidates total 140 (17, 60, 22,
22 and 19 respectively). Each alias has an independent selection proof and
missing-declaration compile mutation. Each family also has a real unmasked
boundary regression killed by runner bypass; the TCU accepted-erase case is
separately identified as preservation evidence.

Final combined `bazel test --config=release` over these five family targets and
`guarded_attempt_expectations_test`, `bound_flash_attempt_test`,
`guarded_can_flash_transport_test` passed **8/8** targets. Final
`python3 scripts/gazelle_check.py --fix` and its buildifier checks passed.
Self-review confirmed no direct-call bypass, no production body/poll changes,
no removed predecessor assertions and no unrelated files staged.

Relative to `0e5ac306`, measured production growth is **+15 NLOC** and test
growth **+279 NLOC**. The test support fixture accounts for **36 NLOC**; family
suite changes account for **243 NLOC**. This is explicit support growth, with
no claimed savings or compression. Full repository delivery gates remain
controller-owned and run once for the completed PR; per-family software proof
does not change any hardware qualification status.

## Denso ISO-15765 N83M 1.5M

Task 7, 2026-10-25; source revision `00d4125d`. Production executor bodies,
profiles, both shared loop polls, UdsClient and clocks remain unchanged.

The shared definition header has an empty default cancellation policy and an
explicit specialization per migrated plan. Inheritance exposes the selected
alias to both explicit instantiation and alias-header users; the shared
executor itself has no blanket guard alias. Direct suite Execute calls use the
conditional adapter. Shared bootloader tests already use ExecutorRun and need
no direct-call replacement. Caller search also covered family conformance and
KernelFreeCanWorkflow bindings in flash_workflow.cpp.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/preparation | Family validation precedes precancel; setup/Open/Close/unblock remain on runner-owned port. No new lifecycle gate. |
| Connect | OBK raw Send/Receive, tolerant branch/access probes, nonfatal identity queries, fatal security and in-car confirmations retain response checks/events. Ten fire-and-forget exchanges directly Read the guarded port, retaining unchecked response ids. |
| Jump/retries | Raw jump Send/Receive and bounded 100 ms sleep/re-read loop retain tolerated exhaustion. N83M 4M/SH72531 retain first-reply discard and 50 ms wait. |
| Read | Both setup exchanges retain fatal/tolerant profile policy. B7 loop retains its poll, decrypt/padding, fixed-page advancement, progress and short-page behavior. SH pacing remains clock-owned; N83M remains unpaced. |
| Write/erase | Shared write-loop poll, absolute image/window offsets, 256-byte chunks, raw erase trigger and 500 ms wait/poll remain. All response validation and abort decisions retained. |
| Stop/close/results | Six best-effort stop/close requests retain ignored cancellation/errors. Checksum remains fatal after its 100 ms wait with profile timeouts. No post-call mapping or final sweep; BoundAttempt closes once with existing precedence. |

Guard entry newly refuses blind-inner Reads at raw OBK/setup/jump/probe,
fire-and-forget and UDS pending continuations. This changes refusal boundaries,
not accepted-operation results. No existing poll is removed.

The existing `CancellationAtTheReadStopIsSwallowed` implements the planned
late-stop characterization (strengthened rather than duplicated). It passed
before opt-in with a cancellation-blind port, exact decrypted/padded image,
zero stop Write attempts and no extra inner Read. UdsClient already refuses all
six stop attempts at entry; full data still returns success. The real-family
BoundAttempt OBK regression instead fails baseline on one extra inner Read and
consumed reply; it is a new guard boundary, separate from preservation evidence.

Focused command: `bazel test --config=release
//src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test`, plus
`//src/backend/flash/ecu:denso_iso15765_bootloader_executor_test`,
`//src/backend/flash/ecu:denso_iso15765_can_common_test`,
`//src/backend/flash/testing:guarded_attempt_expectations_test`,
`//src/backend/flash:bound_flash_attempt_test` and
`//src/backend/flash:guarded_can_flash_transport_test`. Restored candidate passed
**6/6** targets. Baseline XML **29** cases; candidate **31**, with every
predecessor case retained (XML classname/name set comparison), including invalid
plans before precancel and existing family abort cases. Behavior-baseline regressions
expose extra delegated Reads and corresponding result/queued-reply differences;
preserved-behavior characterizations pass. The actual-alias bounded selection
proof fails compilation before the declaration, independently of family polls.
Removing only this specialization's alias again fails compilation. Temporarily
bypassing BoundAttempt guard construction fails both selection and real-family
boundary cases plus the shared bound probe on extra Reads. Both mutations were
restored and the six targets passed again. Gazelle regenerated only named ecu
BUILD dependencies; scoped formatting and diff review passed.

`python3 scripts/nloc_loop.py measure --json`: 153474 / 68449 /
85025 before; 153532 / 68460 / 85072 after
(total / production / test). Production **+11 NLOC**;
test **+47 NLOC**. No compression or savings claimed.
The [qualification matrix](../flash-qualification-matrix.md) still marks this
family experimental; no hardware was used and no dedicated family checklist
is linked from its row. Physical identity, erase/window safety, full-size read,
write/readback, timing and cancellation-boundary bench evidence remain open.
N83M 1.5M additionally retains its recorded stock-read checksum-table gate.
Full repository delivery gates remain controller-owned.

## Denso ISO-15765 N83M 4M

Task 8, 2026-10-25; source revision `541ad561`. Production executor bodies,
profiles, both shared loop polls, UdsClient and clocks remain unchanged.

The preceding shared boundary audit applies. This profile's tolerant setup
continues after wrong/absent content and still propagates transport failures;
its existing malformed-reply, no-frame and bench-discard tests remain. The real
BoundAttempt test cancels after the accepted first 0x34 setup Write with a blind
inner port. Baseline delegates one setup reply Read; the guard refuses it with
`kCancelled`, zero setup-reply Reads and a queued reply. Cancellation is not
converted to tolerated absence. Six best-effort stop/close attempts and all
profile timeout values remain unchanged.

Focused command: `bazel test --config=release
//src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test`, plus
`//src/backend/flash/ecu:denso_iso15765_bootloader_executor_test`,
`//src/backend/flash/ecu:denso_iso15765_can_common_test`,
`//src/backend/flash/testing:guarded_attempt_expectations_test`,
`//src/backend/flash:bound_flash_attempt_test` and
`//src/backend/flash:guarded_can_flash_transport_test`. Restored candidate passed
**6/6** targets. Baseline XML **23** cases; candidate **25**, with every
predecessor case retained (XML classname/name set comparison), including invalid
plans before precancel and existing family abort cases. Behavior-baseline regressions
expose extra delegated Reads and corresponding result/queued-reply differences;
preserved-behavior characterizations pass. The actual-alias bounded selection
proof fails compilation before the declaration, independently of family polls.
Removing only this specialization's alias again fails compilation. Temporarily
bypassing BoundAttempt guard construction fails both selection and real-family
boundary cases plus the shared bound probe on extra Reads. Both mutations were
restored and the six targets passed again. Gazelle regenerated only named ecu
BUILD dependencies; scoped formatting and diff review passed.

`python3 scripts/nloc_loop.py measure --json`: 153532 / 68460 /
85072 before; 153578 / 68465 / 85113 after
(total / production / test). Production **+5 NLOC**;
test **+41 NLOC**. No compression or savings claimed.
The [qualification matrix](../flash-qualification-matrix.md) still marks this
family experimental; no hardware was used and no dedicated family checklist
is linked from its row. Physical identity, erase/window safety, full-size read,
write/readback, timing and cancellation-boundary bench evidence remain open.
N83M 1.5M additionally retains its recorded stock-read checksum-table gate.
Full repository delivery gates remain controller-owned.

## Denso ISO-15765 SH72531

Task 9, 2026-10-25; source revision `2ee97d33`. Production executor bodies,
profiles, both shared loop polls, UdsClient and clocks remain unchanged.

The shared boundary audit applies, including the bench's discarded first reply
and 50 ms wait. The final-page pacing characterization passes before opt-in:
the final page is accepted, then cancellation in its 1 ms Sleep yields
`kCancelled`, zero stop Write attempts and a consumed complete dump script.
The clock remains the owner of that failure; N83M late-success semantics do not
replace it. A separate real BoundAttempt OBK regression fails baseline on an
extra delegated Read and passes with the guard. Existing timing, short-page,
fatal-setup and bench-discard cases remain unchanged.

Focused command: `bazel test --config=release
//src/backend/flash/ecu:subaru_denso_sh72531_can_executor_test`, plus
`//src/backend/flash/ecu:denso_iso15765_bootloader_executor_test`,
`//src/backend/flash/ecu:denso_iso15765_can_common_test`,
`//src/backend/flash/testing:guarded_attempt_expectations_test`,
`//src/backend/flash:bound_flash_attempt_test` and
`//src/backend/flash:guarded_can_flash_transport_test`. Restored candidate passed
**6/6** targets. Baseline XML **22** cases; candidate **25**, with every
predecessor case retained (XML classname/name set comparison), including invalid
plans before precancel and existing family abort cases. Behavior-baseline regressions
expose extra delegated Reads and corresponding result/queued-reply differences;
preserved-behavior characterizations pass. The actual-alias bounded selection
proof fails compilation before the declaration, independently of family polls.
Removing only this specialization's alias again fails compilation. Temporarily
bypassing BoundAttempt guard construction fails both selection and real-family
boundary cases plus the shared bound probe on extra Reads. Both mutations were
restored and the six targets passed again. Gazelle regenerated only named ecu
BUILD dependencies; scoped formatting and diff review passed.

`python3 scripts/nloc_loop.py measure --json`: 153578 / 68465 /
85113 before; 153653 / 68470 / 85183 after
(total / production / test). Production **+5 NLOC**;
test **+70 NLOC**. No compression or savings claimed.
The [qualification matrix](../flash-qualification-matrix.md) still marks this
family experimental; no hardware was used and no dedicated family checklist
is linked from its row. Physical identity, erase/window safety, full-size read,
write/readback, timing and cancellation-boundary bench evidence remain open.
N83M 1.5M additionally retains its recorded stock-read checksum-table gate.
Full repository delivery gates remain controller-owned.

## Denso ISO-15765 SH72543 diesel

Task 10, 2026-10-25; source revision `79556b32`. Production executor bodies,
profiles, both shared loop polls, UdsClient and clocks remain unchanged.

The shared boundary audit applies. DID F182 keeps exactly five identity bytes,
nonfatal identity failures and the tolerant bench session; 2000 ms identity,
probe and in-car profile values remain. The real BoundAttempt test cancels on
an accepted identity NRC 0x78 response. Baseline accepts a further blind-inner
Read; the guard refuses that pending continuation, leaves the positive reply
queued and preserves nonfatal error logging plus VIN/CAL/CVN progression.
The next fatal access-method exchange reports `kCancelled`. It does not turn
a nonfatal identity failure into a new family abort policy. Existing tolerated
bench/identity and page-pacing cases remain.

Focused command: `bazel test --config=release
//src/backend/flash/ecu:subaru_denso_sh72543_can_diesel_executor_test`, plus
`//src/backend/flash/ecu:denso_iso15765_bootloader_executor_test`,
`//src/backend/flash/ecu:denso_iso15765_can_common_test`,
`//src/backend/flash/testing:guarded_attempt_expectations_test`,
`//src/backend/flash:bound_flash_attempt_test` and
`//src/backend/flash:guarded_can_flash_transport_test`. Restored candidate passed
**6/6** targets. Baseline XML **26** cases; candidate **28**, with every
predecessor case retained (XML classname/name set comparison), including invalid
plans before precancel and existing family abort cases. Behavior-baseline regressions
expose extra delegated Reads and corresponding result/queued-reply differences;
preserved-behavior characterizations pass. The actual-alias bounded selection
proof fails compilation before the declaration, independently of family polls.
Removing only this specialization's alias again fails compilation. Temporarily
bypassing BoundAttempt guard construction fails both selection and real-family
boundary cases plus the shared bound probe on extra Reads. Both mutations were
restored and the six targets passed again. Gazelle regenerated only named ecu
BUILD dependencies; scoped formatting and diff review passed.

`python3 scripts/nloc_loop.py measure --json`: 153653 / 68470 /
85183 before; 153704 / 68475 / 85229 after
(total / production / test). Production **+5 NLOC**;
test **+46 NLOC**. No compression or savings claimed.
The [qualification matrix](../flash-qualification-matrix.md) still marks this
family experimental; no hardware was used and no dedicated family checklist
is linked from its row. Physical identity, erase/window safety, full-size read,
write/readback, timing and cancellation-boundary bench evidence remain open.
N83M 1.5M additionally retains its recorded stock-read checksum-table gate.
Full repository delivery gates remain controller-owned.

## Tasks 7–10 aggregate and self-review

Four explicit plan-policy specializations opt in the four Denso ISO-15765
bootloader instantiations; the default policy stays empty. Both shared executor
loop polls, production .cpp bodies, profiles, UdsClient and clocks are unchanged.
All direct calls in the four family suites use the conditional adapter; shared
bootloader tests already use the adapted ExecutorRun helper. No predecessor
case or assertion was removed. Baseline XML totals **100** cases and candidate
XML totals **109** (31, 25, 25, 28); classname/name set comparison lost no cases.

Each family has both an actual-alias synthetic selection proof and an unmasked
real-family BoundAttempt boundary test. All four missing-declaration mutations
fail compilation; all four runner-bypass mutations fail the family's selection
and real-boundary cases plus the shared bound probe. Both mutation sources were
restored before per-family commits. Baseline preserved late-stop/final-pacing
characterizations pass; blind-inner refusal regressions expose intentionally
new boundaries. Diesel's baseline also reaches an unscripted fatal raw Write
and returns Internal after consuming its pending continuation; the guard
refuses the continuation and later fatal Write with cancellation instead.

Self-review additionally asserts N83M 1.5M's total attempted Writes equal the
exact OBK/setup/page prefix. The accepted-operation stop hook alone could miss
an attempted unscripted stop Write. This extra assertion passes with the direct
adapter temporarily bypassed for the late-stop case, as preservation requires;
the adapter was restored. SH72531's final-pacing case already has the same total
attempt count assertion. Shared definition-header includes were ordered during
this review; no code behavior changed.

Final combined `bazel test --config=release` over all four families, shared
bootloader/common suites, `guarded_attempt_expectations_test`,
`bound_flash_attempt_test` and `guarded_can_flash_transport_test` passed **9/9**.
Final Gazelle/buildifier, scoped clang-format, XML preservation review and
`git diff --check` passed. Each task's normal commit hooks also passed.

Final NLOC: 153705 total / 68475 production /
85230 test versus 153474 / 68449 / 85025 at
`00d4125d`. Batch growth: production **+26**,
test **+205** (including **+1 test NLOC** from self-review).
Existing shared support is reused; no support fixture or test compression was
introduced. Full repository/platform delivery gates remain controller-owned.
No hardware qualification status changed; all recorded bench gaps remain open.
