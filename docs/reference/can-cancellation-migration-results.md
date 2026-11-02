# CAN cancellation migration results

Status (2026-11-01): CAN and mixed-CAN software ownership is implemented at
`e9798090bcdf9d0751b9eae55962ae6c9acc27b3`. Interface-selected guarding replaces
executor opt-ins; generic test ownership, typed exposure/reporting and all 33
audited redundant pre-I/O poll removals are complete. The
[flash contract](flash-contracts.md#can-cancellation-ownership) owns runtime
semantics; [ADR 0025](../adr/0025-enforce-cancellation-in-a-guarded-transport.md#durable-completion-contract)
owns the accepted budget and delivery requirements. This record closes the
software migration, while top-of-stack delivery verification remains separate.
No hardware was used and no qualification status advances.

## Shared policy and test ownership

[Production binding](../../src/backend/flash/guarded_flash_execution.cpp) selects
`GuardedCanFlashTransport` for `ICanFlashExecutor` and
`GuardedMixedCanFlashTransport` for `IMixedCanFlashExecutor` after Open. K-Line
receives its original transport. Setup, configuration, Open/Close, restart,
mode/buffer control, unblock and caller-owned lifetimes retain their owners.
Guards refuse new I/O at entry and pass admitted delegate results unchanged,
without a post-call check, final sweep or persistent stopped state.

The shared [exposure and reporting contract](flash-contracts.md#cancellation-exposure-and-reporting)
records quiet typed pre-delegation refusal versus the retained ECU warning for
may-have-started or unknown cancellation, including promotion after successful
Send. That policy belongs to the shared fatal-CAN reporter. NonFatalQuery's
direct error logging/tolerance and raw paths' propagation remain unchanged.
Pre-delegation family detail text is normalized; outcome kind, typed exposure,
accepted I/O and event policy remain the durable test contract.

Generic tests survive at their owners:

- [CAN guard](../../src/backend/flash/guarded_can_flash_transport_test.cpp) and
  [mixed guard](../../src/backend/flash/guarded_mixed_can_flash_transport_test.cpp):
  both CAN and all four raw/ISO entry refusals, typed exposure, forwarding and
  unchanged accepted outcomes.
- [UdsClient](../../src/backend/protocol/uds/uds_client_test.cpp),
  [result owner](../../src/backend/ports/result_test.cpp) and
  [shared reporter](../../src/backend/flash/ecu/uds_client_exchange_common_test.cpp):
  pre-request/failed-Send provenance, delay/Receive/pending-reread promotion,
  quiet refusal, conservative warning and unchanged non-cancellation behavior.
- [BoundAttempt](../../src/backend/flash/bound_flash_attempt_test.cpp):
  alias-free CAN and mixed-CAN raw/ISO selection, unchanged K-Line path,
  lifecycle and Execute/Close precedence.

Family suites retain unique continuation, post-operation checkpoints, error
precedence, transcripts, confirmations and lifecycle behavior. Entry-refusal
cases use cancellation-aware transports or `ExecuteWithTransportGuard` /
`BindFlashAttempt`; undecorated family-only execution cannot establish guard
refusal. Colt keeps separate executor and binding-composition evidence.

## Ownership matrix

`CAN` means the binding-selected `ICanFlashTransport` guard; `mixed` means the
`IMixedCanFlashTransport` guard. Shared dialect rows inherit their caller's CAN
guard. Counts in **parentheses** enumerate the 33 removals at the final poll
layer; inherited removals are counted only at their shared owner. Colt's earlier
four removals and the separately reviewed SH72543R Receive-entry removal are
outside that 33-site count.

Final NLOC is a bounded source census, **production / test**, using pinned
Lizard 1.24.1 and the repository exclusions at the software head above. Each
row counts its named source stem's `.cpp` and `.h`, plus its own `_test.cpp`;
the Denso ISO shared row combines `denso_iso15765_can_common` and
`denso_iso15765_bootloader_executor`. Family rows use the stem of their linked
executor suite. Plan/profile/crypto, generic guard/binding/protocol owners and
test-support files are outside this census; these rows are contributions to the
final repository totals, not per-family deltas or a sum of the whole repository.
Shared tests are counted once even where several families consume them.

Every family remains `experimental`, with no new hardware evidence. The
[qualification matrix](../flash-qualification-matrix.md) owns status and full
limitations; each last-column entry identifies a remaining gap, not a new gate
or a claim that all other bench work is complete.

| Executor / shared dialect | Guard | Removed refusal boundary | Retained family checkpoints | Surviving test owner | Final NLOC P / T | Qualification gap |
| --- | --- | --- | --- | --- | ---: | --- |
| Colt M32R | CAN | Earlier four read/compare polls; next exchange owns refusal | Admission, abort/confirmations, accepted erase, late complete read/verify meaning | [Colt executor](../../src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp) and binding | 471 / 1,630 | Recorded [Colt bench run](../checklists/colt_czt_47110032_can_bench_checklist.md), both capacities and pending behavior |
| Subaru Hitachi M32R ECU | CAN | Write-loop entry **(1)**; B7 through shared owner | Pending continuation, identity tolerance, erase waits, close/checksum policy | [ECU suite](../../src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp) | 288 / 407 | Pending/erase cancellation on named ECU |
| Subaru Hitachi M32R TCU | CAN | ROM-read, raw-erase and block-write entries **(3)** | Accepted erase before next unsent window, reply validation, retry/close/checksum and no-data-after-cancel traces | [TCU suite](../../src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor_test.cpp) | 540 / 2,049 | [Blocking erase-scope bench gate](../checklists/subaru-tcu-hitachi-m32r-can-bench-checklist.md), corrected read window |
| CVT Hitachi M32R | CAN | Write-loop entry **(1)**; B7 through shared owner | Raw erase exposure promotion, security channel, one fatal block close and checksum | [CVT Hitachi suite](../../src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp) | 331 / 553 | Raw erase cancellation and close on named TCU |
| CVT Mitsubishi MH8104 | CAN | Read/write loop entries **(2)** | Successful-Send raw exposure promotion, failed-Send provenance, tolerant checksum | [MH8104 suite](../../src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp) | 401 / 559 | Raw exchange cancellation/checksum on named TCU |
| CVT Mitsubishi MH8111 | CAN | Write-loop entry **(1)**; B7 through shared owner | UDS continuation, twenty-attempt close and fatal checksum | [MH8111 suite](../../src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp) | 275 / 544 | Pending/close/checksum on named TCU |
| Denso ISO N83M 1.5M | CAN | Shared ISO loop entries below | Nonzero-base indexing, in-car/bench policy, best-effort stop may finish successfully after cancellation | [N83M 1.5M suite](../../src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp) | 7 / 559 | Exact dump length, branch timing and stop on ECU |
| Denso ISO N83M 4M | CAN | Shared ISO loop entries below | Tolerant read setup, first-jump discard, best-effort stop/close | [N83M 4M suite](../../src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test.cpp) | 7 / 520 | Jump discard and tolerant setup/close on ECU |
| Denso ISO SH72531 | CAN | Shared ISO loop entries below | SH pacing including final-page cancellation, jump discard, best-effort close | [SH72531 suite](../../src/backend/flash/ecu/subaru_denso_sh72531_can_executor_test.cpp) | 7 / 476 | Final pacing, jump and checksum on ECU |
| Denso ISO SH72543 diesel | CAN | Shared ISO loop entries below | SH pacing, family probe/identity and checksum timeout policy | [SH72543 diesel suite](../../src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor_test.cpp) | 7 / 535 | Pending timeout/identity and pacing on ECU |
| Denso ISO common + bootloader executor | CAN, inherited | Image-write and dump-read loop entries **(2)** | Every wait, SH pacing, best-effort stop/transfer exit, identity tolerance, progress/log order | [Common](../../src/backend/flash/ecu/denso_iso15765_can_common_test.cpp) and [template](../../src/backend/flash/ecu/denso_iso15765_bootloader_executor_test.cpp) suites | 749 / 1,137 | Consumer-family bench evidence |
| UDS exchange / encrypted B7 dialect | CAN, inherited | Encrypted B7 page-loop entry **(1)** | Fatal/nonfatal split, payload checks, decrypt/accumulation/progress order | [Exchange suite](../../src/backend/flash/ecu/uds_client_exchange_common_test.cpp) plus consuming family traces | 184 / 233 | Consumer-family pending and cancellation evidence |
| BEEF CAN kernel | CAN, inherited | ChannelRequest, buffer-write, B6 upload and ROM-read entries **(4)** | Accepted-CRC stale drain, compare before validation/logs, commit before logs/CRC, post-upload raw discard, outer block before voltage/logs | [Kernel suite](../../src/backend/flash/ecu/beef_can_kernel_test.cpp) plus ECU/TCU traces | 548 / 905 | Consumer upload/drain/commit/recovery evidence |
| Denso BEEF ECU execution | CAN, inherited | ChannelRequestOptional entry **(1)** | Post-probe checkpoint, cancellation/disconnection/parse tolerance ordering | Petrol/diesel executor suites below; no separate suite | 708 / 0 | Both ECU families' cold/hot paths |
| Denso SH7058 BEEF petrol | CAN | Shared BEEF/ECU entries above | Startup reset/configure/Open, kernel-probe and family security/abort policy | [Petrol suite](../../src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp) | 59 / 1,629 | B6 discard, security variants and post-erase recovery |
| Denso SH7058 BEEF diesel | CAN | Shared BEEF/ECU entries above | Exact accepted timeout prefix, pending policy, CRC stale drain and family traces | [Diesel suite](../../src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp) | 45 / 1,795 | Both geometries, upload/pending and recovery |
| Denso SH705x BEEF TCU | CAN | NonfatalQuery, trailing-fragment drain and kernel-ID probe entries **(3)** | Bounded ID coalescing, exact session validation, restart and wait, preflight; unused polling helper deleted | [Denso TCU suite](../../src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp) plus restart integration | 442 / 2,126 | Kernel-ID, restart and supported variants on TCU |
| Hitachi SH7058 | CAN | Session Exchange and erase-continuation Read entries **(2)** | Successful Write/delay and Read checks, delegated Read-error precedence, 20-read continuation bound, retries and reply interpretation | [SH7058 suite](../../src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor_test.cpp) | 333 / 310 | Exchange/erase/program/finalization on ECU |
| Hitachi SH72543R | CAN | Session Send entry **(1)**; earlier separate Receive-entry deletion | Both Sleep checks, successful-Write check, unconditional post-Read cancellation precedence, completion/finalization, accepted erase/retry and encryption admission | [SH72543R suite](../../src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor_test.cpp) | 455 / 886 | Erase/retry/close/checksum and cancellation on ECU |
| Denso SH705x CAN EEPROM | CAN | Raw exchange Write, kernel chunk and ReadPageData entries **(3)** | Post-Write/delay/Read, post-upload settle, request/header ACK, page post-Read before accumulation, connect-log suppression, admission before empty-kernel validation, outer page computation | [EEPROM suite](../../src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor_test.cpp) | 649 / 983 | SH7058 upload address and all EEPROM modes on hardware |
| Subaru Denso SH705x DensoCAN | mixed | Four raw/ISO helper entries; wake, upload, ROM-read and buffer-write loops **(8)** | All four post-I/O checks, transport-error precedence, padding/overflow/log and compare boundaries, reset/configure/Open, mode/buffer lifecycle, post-erase recovery warning | [DensoCAN suite](../../src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor_test.cpp) and binding | 920 / 1,701 | Raw/ISO cancellation, restart and recovery on hardware |

The 33 removals span 14 production files. Only admitted request/frame/address
preparation intervenes before the next guarded or UdsClient-owned I/O. No wait,
log, event, confirmation, validation or family computation boundary was removed.
SH72543R's earlier Receive-entry deletion preserves its unconditional post-Read
checkpoint before Read-error inspection and is not counted again. BEEF and ECU
execution retain polling helpers used by their remaining non-I/O checks.

## Measurements and accepted budget

All rows below use refreshed current-master trunk and Lizard 1.24.1 exclusions through
`python3 scripts/dedup_loop.py measure --json`. Historical rollout measurements
from other trunk revisions are not subtracted from this baseline.

| Layer / source | Production NLOC | Test NLOC | Total |
| --- | ---: | ---: | ---: |
| Current-master trunk `44c33ab52b0efc1827f01c333c998e1bae58c67d` | 66,211 | 88,889 | 155,100 |
| Conservative reconstructed rollout `b952b92b` | 66,368 | 90,529 | 156,897 |
| Interface binding + shared-test compression `97ad050a` | 66,332 | 89,780 | 156,112 |
| Typed exposure/reporting `6967fa36` | 66,347 | 89,966 | 156,313 |
| Final poll ownership `e9798090` | **66,214** | **89,960** | **156,174** |
| Final minus current-master trunk | **+3** | **+1,071** | **+1,074** |

Production is one NLOC below its approved 66,215 hard maximum (`+4`); tests remain 30
NLOC below their 89,990 maximum (`+1,101`). Total maximum is 156,205; actual
156,174 leaves 31 total NLOC of slack. This work did not achieve
net-negative test NLOC; the approved bounded-growth budget replaced that goal.
Compression and poll deletion reduce the transitional rollout, while the final
stack still grows against trunk. The independently green exposure layer also
corrected the Hitachi TCU's stale next-unsent-window warning expectation, making
its test total one below the original forecast. Its later whole-error matcher
repair removes five more test NLOC without changing the asserted kind, detail or
exposure, leaving the repaired layer six below that forecast. From that actual
base the poll layer removes 133 production and six test NLOC, including the
separately reviewed SH72543R Receive removal; the disposable spike's different deltas are not claimed.
The final-review cleanup removes 25 duplicate test NLOC from each affected
layer; it preserves the approved relative maxima.
The previous pinned baseline `e62684af` (66,405 / 88,783) and repaired final
66,409 / 89,879 are historical measurements, not the current baseline.

The refreshed local stack is linear: decision `c01938a0`, reconstructed #908
`b952b92b`, binding `58afcaf1`, shared-test compression `97ad050a`, exposure
`6967fa36`, poll ownership `e9798090`, then closeout. Closeout retained its eight
patch-equivalent commits through `3e15bbff`; the separate documentation refresh
containing this record is its new head on
`docs/can-cancellation-migration-closeout`. The full final head is available from
that commit in Git history without a self-referential SHA in its own contents.
The local metadata base is `stack/can-cancellation-current-trunk`, exactly the
measured master SHA above; this synthetic name is not a remote PR base.

## Software evidence and delivery limits

Linux LLVM 23.1.4 clang-tidy reported one uninitialized assignment in the MH8104
Receive `transform_error` path at exposure `063922f8` (run 41442210965, job
125266750336). The repair uses an explicit checked Result and preserves the
Send and Receive exposure boundaries. A retry exit and kernel-alive prefix check
were simplified equivalently to remain within the production budget. The Linux
CI rerun remains outstanding; local verification does not establish its result.

The reconstructed rollout preserves its predecessor protocol tests. Binding
selection and shared-owner compression then remove redundant generic evidence;
family continuation, precedence and checkpoint cases survive. Poll deletion was
characterized before removal and checked in three focused batches. BEEF retains
exact accepted script consumption while allowing a refused Send attempt; diesel
and SH72543R sweeps exercise the production guard seam. Only the deleted private
BEEF entry-detail test is removed at the poll layer; no fixture or suite is
removed there. Hitachi TCU loop detail assertions become kind-based while full
traces and accepted I/O counts remain.

At the original exposure head `e90c442e`, the eight focused exposure targets and
the additionally aligned Hitachi TCU target passed; the full release suite passed 306 targets with 20
configured fuzz/platform skips. At the original poll head `69d48de1`, all 88
flash targets and the full release suite passed (306 pass / 20 skip); Gazelle converged without BUILD
changes and all applicable Prek hooks passed. The original final mutation run
selected all 22 dependent targets from `e90c442e...69d48de1`: all completed, with no mutants
in changed-line scope and zero survivors. Pure deletions supply few mutable
changed lines; this is not mutation coverage of removed instructions. The
characterizations, retained-boundary audit and exact accepted-I/O traces provide
the deletion evidence.

Historically, repaired exposure head `0f9466ef` passed all five affected owner targets. The
repair run reported focused clang-tidy (36 files, zero findings). The then-restacked
poll head `05d51398` retained the original poll layer byte-for-byte above that
test-only repair.
Whole-error matchers replace direct error-field assertions and preserve refusal,
promotion and warning evidence; no production or analysis policy changed. Full
clang-tidy on the restacked source was reported by the repair run as 552 files
and zero findings. These clang-tidy results are prior reported evidence: raw
logs and exact timings did not survive into recovery, which did not rerun
clang-tidy.

The current-master refresh ran 90 focused flash, UDS and result-owner test
targets on closeout `674ab4f8`: all passed, with 70 re-executed and 20 cached.
Gazelle generation and convergence checks produced no tracked BUILD changes,
and all applicable Prek hooks passed. The documentation refresh preserves that
source tree. Conflicts were limited to generated BUILD entries: current-master
family targets/dependencies survive alongside the guard ownership changes.
Stable patch IDs match for 35 of 38 original commits. The remaining three
range-diff differences reflect current-master includes and BUILD target placement;
their added and removed lines retain the original changes.
No production/test conflict required a behavioral change.

Full post-rebase verification at source head
`3299167bd041db5714716c0a7ca5f35bb2beda07` passed the release application build,
release tests (306 pass / 20 configured skips), all 17 Prek hooks, convergent
Gazelle generation and clang-tidy (553 files / zero findings). Coverage also
passed 306 targets with 20 configured skips; the combined LCOV artifact is
preserved in the verification evidence. These counts do not assert a line
coverage percentage. The skips are thirteen fuzz and seven Windows-only targets
on the macOS verification host.

The exact unscheduled changed-line mutation command against base
`44c33ab52b0efc1827f01c333c998e1bae58c67d` failed when the original instrumented
SH72543R suite exceeded its 60-second warmup limit, before that binary executed
mutants. That failed attempt remains evidence. Systematic diagnosis showed that
the four checked-in GoogleTest shards cover all 189 cases exactly once, each
passing in roughly 17–18 seconds. A fresh invocation with the checked-in
schedule selected the same 161 labels as 176 work items; all completed, with
2 killed / 0 survived / 0 uncovered / 0 timed-out changed-line mutants. Loading
the schedule is a disclosed, coverage-preserving execution correction: selected
labels, changed-line scope and test cases remain intact, without increasing
timeouts or changing filters. The successful verdict uses only the fresh run's
reports, not a merge with the failed partial attempt.

The earlier exact NLOC (66,215 production / 89,985 test), exact-base diff, linear topology,
local ref and remote identity checks passed at source head `3299167b` before
this documentation-only closeout. Its final documentation-only commit leaves
source, tests and the build graph identical to that verified head. Final review
subsequently removed a standalone diesel short-security-key reply test whose
setup, execution and assertions are identical to the short-reply row retained
in the two-case test. The production source and graph remain unchanged. The
mutation results above are historical, not a fresh run after this cleanup.

Independent final whole-stack review, platform CI and packaging, hardware
qualification, and explicit authorization for remote submission, opening or
readiness, and merge remain outstanding. No Linux/Windows CI result is claimed;
local skips do not establish Windows qualification.
The [qualification matrix](../flash-qualification-matrix.md) and applicable
bench checklists remain unchanged, including Colt's recorded bench gate and
the blocking Hitachi TCU erase-scope question.

## Historical evidence

The former migration diary is recoverable at
`05d51398:docs/reference/can-cancellation-migration-results.md`; it records the
conservative opt-in rollout rather than the completed ownership model. The
[Colt results](colt-cancellation-centralization-results.md) and
[pilot lessons](guarded-transport-pilot-lessons.md) remain durable evidence.
Completed plans/designs were preserved in lower decision/rollout commits before
closeout deletion. Use the
[history recovery instructions](../README.md#recover-completed-work-and-deleted-source-citations)
to inspect them. Preserving those lower layers in delivered Git history remains
necessary; squashing their introduction and deletion into a single commit would
lose their original contents.
