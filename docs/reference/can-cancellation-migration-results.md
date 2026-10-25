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

## Denso SH7058 BEEF petrol

Task 11, 2026-10-25; source revision `5b5dd5e8`. The concrete executor now
selects `GuardedCanFlashTransport`; all direct suite Execute calls use the
conditional adapter. Production executor/helper bodies and all polls remain.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/preparation | Family validation precedes precancellation. The three petrol reset/configuration polls, reset, quiet period, configure/open and close remain on the owned port. |
| Connect | Tolerant kernel/identity/session queries retain terminal cancellation/disconnect and nonfatal malformed/stale failures. RaceRom RAM/security, strict UDS pending bounds and clocks remain unchanged. Shared request and post-probe polls remain. |
| Upload | Upload-loop and discard-entry/after-read polls remain. Accepted B6 Write followed by cancellation now refuses the raw discard Read before delegation; baseline delegated one blind-inner Read before the retained after-read poll returned cancellation. No next B6 or transfer-exit follows either outcome. Ordinary malformed/wrong-ID/timeout/adapter discard results remain ignored. |
| Read/CRC | BEEF validation, raw ROM bytes, page/compare polls, range guards, progress and 5 ms compare waits remain. CRC stale drain retains its pre-read poll and tolerance of ordinary stale errors; cancellation/disconnect are terminal. |
| Flash/final | Initialization, erase, write/block/commit polls, recovery warnings, confirmation decisions and final completion remain with their current owners. Guard only refuses new Read/Write; accepted operations, post-operation checks and error precedence remain unchanged. |

The real BoundAttempt B6 test records zero attempted discard Reads after the
accepted Write and no subsequent attempted Write. It leaves the discard queued
and closes once. Cancellation still returns `kCancelled`; its diagnostic now
comes from guard entry (`CAN read cancelled before delegation`) instead of the
baseline after-read upload poll. The accepted-write prefix and upload notice/logs
remain; upload progress after the discard and later kernel/read events do not
occur. Existing exact transcript/log/progress and ignored-reply tests pass.
The actual-alias synthetic selection test independently proves runner wiring.

Verification (`bazel test --config=release`): family target plus
`beef_can_session_test`, `denso_beef_can_common_test`,
`//src/backend/flash/testing:guarded_attempt_expectations_test`,
`//src/backend/flash:bound_flash_attempt_test` and
`//src/backend/flash:guarded_can_flash_transport_test` passed **6/6**. Baseline
preservation suite passed **28/28**; candidate XML passed **30/30** and comparison
of classname/name sets lost no predecessor case. Raw-discard regression RED
failed with one extra delegated Read; missing-alias RED failed compilation with
`no type named CancellationGuard`. Removing the alias reproduced that failure;
bypassing BoundAttempt guarding failed both new family proofs and the shared
bound probe. Restored sources passed all six targets. Logs/XML are under
`/private/tmp/beef11-*`; Gazelle and scoped formatting passed, followed by scoped
self-review and `git diff --check`.

`python3 scripts/nloc_loop.py measure --json`: baseline
153705 total / 68475 production / 85230 test; final
153753 / 68477 / 85276. Growth: production **+2**, test **+46**; existing support
reused, no compression or new fixture framework. The qualification matrix keeps
this family experimental. No dedicated BEEF CAN checklist exists; connection
bench prerequisites were consulted. Named adapter/ECU traces, read comparison,
write/test-write/verify/recovery and cancellation at accepted B6 remain bench
gaps; these software tests establish no hardware qualification. Full repository,
static analysis and platform delivery gates remain controller-owned.

## Denso SH7058 BEEF diesel

Task 12, 2026-10-25; source revision `fb6c3450`. The diesel header explicitly
selects the CAN guard; direct suite calls use the adapter. No production body,
shared helper, startup or execution poll changed.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/preparation | Plan admission remains first; the startup reset poll, ResetConnection and cancellable 500 ms quiet period remain on the owned port before configure/open. Close remains runner-owned. |
| Connect | Tolerant kernel probe, identity failures and 03/43 session fallback retain their error/log decisions. Strict security and UDS pending retry limits, timings and NRC diagnostics remain unchanged. A cancelled pending Read now stops at guard entry rather than reaching the recording/inner port. |
| Upload | Shared upload-loop, discard-entry and after-read polls remain. B6 raw discard (500 ms for diesel) newly refuses delegation after an accepted Write cancels. Ordinary timeout/stale/malformed/wrong-ID content stays ignored; cancellation/disconnect remain terminal. No later B6/37 is attempted. |
| CRC | The retained pre-drain checkpoint already refuses the stale Read after cancellation from the CRC reply. The independent new characterization passes on the unguarded baseline and candidate with zero stale Read attempts; it is preservation, not a guard bypass proof. CRC result/progress logs after this checkpoint remain absent. |
| Read/flash/final | Both generation geometries, page/range checks, compare/read/block/write/commit polls, compare waits, erase/commit recovery warnings and final result policy remain. Clocks and UdsClient are untouched. |

The real-family BoundAttempt B6 regression fails baseline on one delegated raw
Read, then passes with none and a queued discard reply. It returns cancellation
and closes once; diagnostic changes from `kernel upload cancelled` at the
retained after-read poll to `CAN read cancelled before delegation`. Accepted
upload-prefix events remain; subsequent upload progress/start/read events do
not occur. The actual-alias synthetic probe separately proves selection.

One existing pending-cancellation assertion necessarily changes its observation
of attempted transport calls: its configured pending timeout stays **3000 ms**,
but the cancelled pending Read is no longer delegated. The exact delegated
prefix is now 800 ms followed by seven 2000 ms Reads; 2000 ms is the preceding
successful delegated Read, not a changed pending policy. Its cancellation
result/no-resend assertions remain and the queued positive reply is now asserted
unconsumed. Ordinary pending-retry tests still assert the 3000 ms policy and
bounded retry count. All other predecessor assertions remain.

Verification: unchanged baseline **35/35**; added stale-drain preservation case
passes before opt-in. Candidate XML **38/38**, no lost classname/name pair.
Missing-alias RED and declaration removal fail compilation; raw B6 RED fails on
one extra Read. Runner bypass fails family selection and real B6 cases and the
shared bound probe. Sources restored, family plus `beef_can_session_test`,
`denso_beef_can_common_test`, `guarded_attempt_expectations_test`,
`bound_flash_attempt_test` and `guarded_can_flash_transport_test` pass **6/6**
using `bazel test --config=release`; exact-prefix follow-up also passes.
Gazelle, scoped formatting, scoped diff review and `git diff --check` pass.
Evidence: `/private/tmp/beef12-*`.

`python3 scripts/nloc_loop.py measure --json`: baseline
153753 total / 68477 production / 85276 test; final
153841 / 68479 / 85362. Growth: production **+2**, test **+86**, with existing
shared support and no compression. Qualification remains experimental; no
BEEF CAN-specific checklist exists, and connection prerequisites were consulted.
Named SH7058d/SH7059d adapter traces, read/write/test-write/verify/recovery and
cancellation at upload/pending boundaries remain bench gaps. Full repository,
static and platform gates remain controller-owned.

## Denso SH705x BEEF TCU

Task 13, 2026-10-25; source revision `450e97a3`. Explicit public CAN guard
selection and direct-call adapter routing are the only production interface/test
execution changes. Production bodies, shared helpers, polls, clocks, UdsClient
and actual TCU restart integration remain intact.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/preflight | Family validation precedes transport work and cancellation. Read-only preflight still belongs to the UI: action choice, ignition confirmation for service actions, and dump handoff are unchanged. Backend plans reject extra confirmations; guard does not own those choices. |
| Kernel ID | Five probe attempts, probe-entry polls and 100 ms sleeps remain; raw initial/continuation Reads use the guarded port. Each trailing drain retains its poll and 32-frame bound; malformed/wrong-ID fragments and timeout/empty completion retain their rules. Existing terminal cancellation/disconnect and no-next-command tests remain. |
| Session/upload | Exact session/security validation and no-upload-after-failure stay family-owned. Shared upload-loop and discard-entry/after-read polls remain. Accepted B6 cancellation newly refuses the raw 2000 ms discard Read; ordinary ignored replies remain nonfatal. No subsequent B6 or 37 is attempted. |
| Restart | Executor-owned RestartIso15765 still forwards to the actual inner override, resetting/reconfiguring/reopening before the 500 ms quiet period and strict post-upload probe. No guard gate or substitute restart fixture added. Existing integration tests assert order/configuration and reset/configure/open failures with outer Close. |
| Read/CRC/write/final | Raw read pages, both MCU geometries, shared CRC stale pre-drain/compare polls, local read/write/commit/block polls, range guards, waits, recovery warnings, compare policy and final result remain. No final cancellation sweep. |

The real-family BoundAttempt test uses the suite's SH7055 read plan and accepted
B6 Write hook. Baseline attempted one discard Read; candidate attempts zero,
leaves its reply queued, attempts no following Write and closes once. Both
return cancellation, but guard entry now provides `CAN read cancelled before
delegation` instead of the upload after-read poll. Accepted-prefix upload logs
and preparation notice remain; subsequent transfer-exit/start/restart/read
work and events do not occur. Synthetic actual-alias proof independently tests
selection and runner wiring. Existing content tolerance, trailing-drain terminal
errors, exact session validation and complete restart cases all still pass.

Verification: baseline XML **33/33**, candidate **35/35**, no lost classname/name
pair. Raw B6 baseline RED fails on one extra Read. Missing-alias RED and temporary
declaration removal fail compilation. Temporary runner bypass fails both new
family proofs and the shared bound probe. Restored family and shared targets
(`beef_can_session_test`, `denso_beef_can_common_test`,
`guarded_attempt_expectations_test`, `bound_flash_attempt_test`,
`guarded_can_flash_transport_test`) pass **6/6** with
`bazel test --config=release`. Final Gazelle/buildifier, scoped formatting,
scoped diff review and `git diff --check` pass. Evidence: `/private/tmp/beef13-*`.

`python3 scripts/nloc_loop.py measure --json`: baseline
153841 total / 68479 production / 85362 test; final
153888 / 68482 / 85406. Growth: production **+3**, test **+44**. Existing shared
blind transport and selection proof are reused. No new support framework,
compression or EEPROM restart test. Experimental hardware status remains;
TCU session-safety validation, named adapter/TCU read/write/verify/recovery,
restart behavior and accepted-B6 cancellation need bench traces. No dedicated
BEEF CAN checklist exists; connection prerequisites and matrix limitations
were consulted. Full repository/static/platform gates remain controller-owned.

Final formatted diesel measurement correction: its exact-prefix assertion
formats to one fewer NLOC than the intermediate measurement. Task 12 final is
153841 / 68479 / 85362, for production **+2**, test **+86**. The three-task batch
therefore grows production **+7**, test **+176** from `5b5dd5e8`; this is support
and regression coverage growth, not savings.

## Hitachi SH7058 CAN

Task 14, 2026-10-25; source revision `24265031`. The public alias explicitly
selects `GuardedCanFlashTransport`; all direct suite Execute calls use the
existing adapter. Production `.cpp` and helper bodies and every poll remain
unchanged. Workflow callers use `KernelFreeCanWorkflow` and BoundAttempt.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/setup | Family validation and write-only admission precede precancel; configuration, Open, Close and unblock stay runner-owned. |
| Exchange/connect | Session checkpoint before Write, after optional cancellable delay and after successful Read remains. Timeout-to-empty mapping and delegated transport error handling precede the last checkpoint. Read failure therefore still wins over concurrent cancellation; a successful Read still returns `SH7058 write cancelled`. Bench/in-car identity, security and session validation remain. |
| Erase | Initial exchange and all 20 follow-up Read entry checks remain; positive reply, transport failure and retry exhaustion decisions are unchanged. |
| Program/final | Payload encryption, full 4096-frame loop, required F6 replies, six transfer-setup attempts, twenty close/checksum attempts, progress and 100 ms finalization delay remain. No final sweep added. |

The real-family BoundAttempt successful-Read case observes one accepted probe
Write and one Read, then cancellation at the retained post-read checkpoint,
no erase/data commands and one Close. It passes unguarded and guarded: this
is preservation evidence, not bypass detection. The simultaneous failed-Read
case uses the package-owned blind transport's narrow optional failure hook;
`QueueError(kDisconnected, "read failed")` cancels before returning that error.
Its accepted-Read hook behavior remains unchanged; no duplicate fixture added.
The actual-alias synthetic selection probe independently detects bypass on one
extra delegated Read. No unmasked deterministic real-family boundary was found:
the retained entry/after-call checks already protect its Read paths. Guard entry
still closes the concurrent interval after the last poll. No observed family
error/log/progress metadata changes are claimed.

Verification: baseline XML **9**, candidate **12**, no predecessor classname/name
pair lost. Both preservation cases pass before opt-in, including rerun with the
final shared failure hook and absent alias (selection case temporarily omitted).
Missing-alias selection RED and declaration removal fail compilation; runner
bypass fails the family selection case and shared probes on the extra inner Read.
After restoration, `bazel test --config=release` passes the family and
`guarded_attempt_expectations_test`, `bound_flash_attempt_test` and
`guarded_can_flash_transport_test` (**4/4**). Gazelle regeneration, scoped
clang-format and diff review pass. Logs/XML: `/private/tmp/postop14-*` and
`/private/tmp/postop-subaru_hitachi_sh7058_can-baseline.xml`.

`python3 scripts/nloc_loop.py measure --json`: baseline
153888 / 68482 production / 85406 test; candidate 153948 / 68485 / 85463.
Growth is production **+3**, test **+57**, including **5** shared hook NLOC.
No compression or savings claimed. Qualification matrix and connection checklist
consulted; no dedicated SH7058 CAN checklist exists. Hardware remains
experimental: named ECU/adapter traces, read/write verification, recovery and
cancellation at exchange/erase/program/finalization boundaries remain bench gaps.
Full repository/static/coverage/platform gates remain controller-owned.

## Hitachi SH72543R CAN

Task 15, 2026-10-25; source revision `320fd021`. Public guard opt-in and
adapter routing retain every production body, checkpoint, clock and validation.
Workflow callers use `KernelFreeCanWorkflow`/BoundAttempt; all direct suite
Execute calls use the shared adapter.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/setup | Plan and wire validation, unsupported test-write, precancel, preparation/configuration/Open and close-once ownership remain. |
| Send/Receive/Sleep | Send checks before and after Write; Receive checks before and after Read, with cancellation winning over a concurrently returned error. Sleep checks before/after its cancellable clock call. These checks mask deterministic bypass at accepted I/O boundaries; guard still refuses the concurrent interval after the last entry poll. |
| Connect/read | Optional identity/session content tolerance, exact seed/key and 1024-byte page validation, six nonfatal stop attempts, post-read completion checkpoint, identity metadata and progress remain. |
| Encrypt/erase | Pre-encryption checkpoint, absolute image offsets, protected address window, one erase Write with 21 Reads, pacing and after-positive-erase checkpoint remain. |
| Program/final | All 8096 B6 frames and tolerant data response content, per-exchange checkpoints, after-program check, independent twenty-attempt close/checksum retries and final completion checkpoint remain. |

`CancellationStillWinsOverConcurrentReadFailure` uses the shared blind port's
failure hook with exact delegated `kDisconnected / read failed`; the token is
asserted cancelled and the family still returns `kCancelled / flash cancelled`
after one Write/Read, without another request. Real-family BoundAttempt
accepted-Write cancellation retains that same diagnostic, zero Reads, a queued
reply and one Close. Both pass unguarded: they preserve family policy and do
not prove bypass detection. The actual-alias synthetic selection proof provides
that detection. No unmasked deterministic real-family boundary or changed
family refusal metadata was observed. Complete-write, tolerated failures,
validation, finalization retry and every predecessor assertion remain.

Verification uses `bazel test --config=release` for the family plus
`guarded_attempt_expectations_test`, `bound_flash_attempt_test` and
`guarded_can_flash_transport_test`: **4/4** pass after restoration. Baseline
XML **38**, candidate **41**; no predecessor classname/name pair lost.
Missing-alias RED and declaration mutation fail compilation; temporary runner
bypass fails family selection and shared probes on one extra delegated Read.
Sources restored; Gazelle, scoped clang-format and diff review pass. Evidence:
`/private/tmp/postop15-*` and
`/private/tmp/postop-subaru_hitachi_sh72543r_can-baseline.xml`.

`python3 scripts/nloc_loop.py measure --json`: baseline
153948 / 68485 production / 85463 test; final 154000 / 68488 / 85512.
Growth: production **+3**, test **+49**; existing support reused, no compression.
Qualification matrix and connection prerequisites consulted; no dedicated
SH72543R checklist exists. Hardware remains experimental. Named ECU/adapter
read/write/cancellation traces, recovery, physical erase scope/block-0
preservation and programmed-image readback remain bench gaps. Full repository,
static, coverage and platform gates remain controller-owned.

## Denso SH705x CAN EEPROM

Task 16, 2026-10-25; source revision `4e1a8220`. Explicit public guard alias
and adapter routing retain all production `.cpp`/helper bodies and polls.
The suite's test-only invocation helper now borrows `const ICancellationToken&`
so ManualCancellationToken works alongside the existing default fake; no
production API changes. Workflow BindAdmittedAttempt keeps EEPROM retry staging.

| Boundary | Retained owner and guard effect |
| --- | --- |
| Admission/connect | Invalid plan still wins over precancel. Connect entry, CanRawExchange before/after Write, after delay and successful Read polls remain; delegated errors still precede after-call cancellation. Identity content tolerance, seed/key/session validation and kernel-alive shortcut remain. |
| Upload | Upload-entry check, size/checksum/encryption preparation, gated SID34 reply, each chunk-entry and after-Write/Read poll remain. The last B6 frame is still header-only. Start 37/31 acknowledgments, settle delays and single strict post-upload probe remain. |
| EEPROM page | Outer loop check, after-request and after-header polls, page accumulation pre/post checks, 100-poll short-page tolerance, framing stripping, address arithmetic, progress and 1 ms inter-page delay remain. |
| Lifecycle/workflow | Setup/Open/Close/unblock remain runner-owned; the guard forwards actual restart without a new gate, but EEPROM never invokes restart. Existing guard-owner forwarding and actual BEEF TCU integration tests cover restart; no invented EEPROM restart fixture. Modes 2/3/4, operator preview/discard and ignition staging stay workflow-owned. |

The predecessor `CancellationDuringKernelUploadReturnsCancelled` had an
inaccurate comment: instrumenting its unchanged token-aware baseline proves
`CancelOnCheck(75)` actually stops after accepted session-set Write **11**
(`00 00 07 e0 10 02 42`), before its Read or SID34. There are **10** delegated
Reads; the last reply is seed-key ack `00 00 00 00 67 02`. Result detail is
`cancelled after write`; logs end at `Set session mode`, with no progress or
upload events. This disagreed with the initial brief's accepted-SID34 description
and was reported before reconciliation. The controller authorized preserving
the measured boundary. The predecessor name and eleven-Write assertion remain;
ManualCancellationToken now cancels at that accepted session-set Write and
pins the trigger, exact reply/error/log prefix and absence of SID34/B6.
Observation log: `/private/tmp/postop16-observe.log`; temporary instrumentation
was removed without changing production.

Separate deterministic preservation cases cancel after the accepted SID34 reply
(**12 Writes / 12 Reads**, `cancelled after read`, no first B6) and after an
accepted B6 Write (**13 Writes / 12 Reads**, `cancelled after kernel chunk write`,
zero chunk-ack Read attempts, no subsequent Write, one Close via BoundAttempt).
All three pass unguarded and guarded. Retained post-operation checks mask guard
bypass at those boundaries; these are family preservation evidence. The
actual-alias synthetic selection proof alone exposes the extra delegated Read
when runner construction is bypassed. No unmasked deterministic real-family
boundary or changed refusal metadata was found; the guard additionally covers
the concurrent interval after a retained entry poll. No polls were removed.

Verification: baseline XML **13**, candidate **16**, no predecessor classname/name
pair lost. Selection RED and declaration mutation fail compilation on the absent
alias; runner bypass fails family selection and shared probes on one extra Read.
After restoration the family and three shared targets pass **4/4** using
`bazel test --config=release`. Final combined gate includes both Hitachi suites,
EEPROM, `guarded_attempt_expectations_test`, `bound_flash_attempt_test`,
`guarded_can_flash_transport_test`, actual
`subaru_tcu_denso_sh705x_can_executor_test` and `flash_workflow_test`: **8/8** pass.
Final Gazelle, scoped clang-format and diff review pass. Evidence:
`/private/tmp/postop16-*`, `/private/tmp/postop-final-combined.log` and
`/private/tmp/postop-denso_sh705x_eeprom_can-baseline.xml`.

`python3 scripts/nloc_loop.py measure --json`: baseline
154000 / 68488 production / 85512 test; final 154105 / 68491 / 85614.
Growth: production **+3**, test **+102**. Batch Tasks 14–16 therefore adds
production **+9**, test **+208**, including the five-NLOC shared failure hook;
this is support/regression coverage growth, not savings or compression.
Qualification matrix and connection prerequisites consulted; no dedicated CAN
EEPROM checklist exists. Hardware remains experimental. Named adapter/EEPROM
read comparisons, modes/ignition retry, upload/cancellation traces and the
SH7058 kernel load address `0xFFFF3000` require bench evidence. Full repository,
static, coverage and platform delivery gates remain controller-owned.
