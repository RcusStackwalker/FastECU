# Colt cancellation centralization results

Status: Colt-only implementation merged in
[PR #770](https://github.com/RcusStackwalker/FastECU/pull/770); local build, test,
analysis, coverage and measurements passed; fresh branch review found no issues.
Delivery CI passed on `6d067a46e760daae7c7b420186943274c0151c97`.
Local Mull produced no mutants and establishes no
automatic mutation coverage. No hardware qualification is claimed.

Implementation base: `883b2afb89464e576ab9a07cdd3442dc997e2130`.
Spec baseline: `2b9613b8275a35fa485caa757a05ba628f163744`.
No drift in Colt implementation or runner header between these revisions.

## Conditions

macOS-27.0.1-arm64-arm-64bit-Mach-O

Apple M5 Pro, 15 logical CPUs, 24 GiB memory; macOS 27.0.1 arm64.
Apple clang 21.0.0 (clang-2100.3.34.2); Bazel 9.1.1; pinned Qt 6.8.3.
Lizard 1.24.1 with unchanged exclusions. Static analysis uses Homebrew LLVM
23.1.3 (CI pins 23.1.2); Mull nightly uses LLVM 22.1.8.
Release configuration, uncached tests, one local test job, no competing builds.
One warm-up followed by five measured runs; execution time comes from target XML, excluding build time.

## Frozen runtime manifest

```sh
PILOT_BASE_SHA=883b2afb89464e576ab9a07cdd3442dc997e2130
PILOT_TESTS=(
  //src/backend/flash:flash_executor_test
  //src/backend/flash:bound_flash_attempt_test
  //src/backend/flash:can_flash_uds_channel_test
  //src/backend/flash/ecu:mitsu_colt_m32r_can_plan_test
  //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test
  //src/backend/flash/ecu:uds_client_exchange_common_test
  //src/backend/flash/testing:scripted_flash_transports_test
  //src/backend/protocol/uds:uds_client_test
  //src/backend/ports/testing:fake_clock_test
)
```

## Baseline source hashes

| Path | SHA-256 |
| --- | --- |
| `src/backend/flash/flash_executor.h` | `7020f428ac7abfc7cd7b5098c512b848aa920dd0b01a0ff11656565510af82ac` |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp` | `e20a4aa071bee9cdaaca0a4e66ad2ec625c5e33fea68917f9626ff89d7235c51` |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h` | `75a88e3ee6095598b8a0baed4e8bc49399097c248f96e49bb74385cf996abfe1` |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp` | `f7845bbd8ceeba09179e02acc8c5136849f4f530094dea9d760b67f11456c763` |

## Baseline poll sites

Colt implementation lines 245 (ReadFlashRange), 287 (FlashRangeMatches),
418 (UnlockAndErase), and 678 (Execute).

## Retained predecessor XML cases

```
MitsuColtM32rCanExecutor.ACancellationThatArrivesAfterTheRequestStopsBeforeTheReply
MitsuColtM32rCanExecutor.ACancelledEraseTriggerIsNotReportedAsAnEcuRejection
MitsuColtM32rCanExecutor.AFailedTransportWriteIsReportedWithoutWaitingForAReply
MitsuColtM32rCanExecutor.AbsorbsResponsePendingWithoutResending
MitsuColtM32rCanExecutor.FailsWhenTheEcuPendsPastTheRepeatLimit
MitsuColtM32rCanExecutor.HandshakeRejectsAReplyTooShortToHoldAServiceByte
MitsuColtM32rCanExecutor.ReadEmitsMonotonicProgress
MitsuColtM32rCanExecutor.ReadPropagatesADisconnectedTransport
MitsuColtM32rCanExecutor.ReadRejectsAChunkAnsweredWithTheWrongService
MitsuColtM32rCanExecutor.ReadReportsAnEmptyReplyAsTimeout
MitsuColtM32rCanExecutor.ReadReturnsEachProtocolCapacityFromAddressZero
MitsuColtM32rCanExecutor.ReadStopsAtTheNextChunkWhenCancelledMidRead
MitsuColtM32rCanExecutor.ReadStopsWhenCancelled
MitsuColtM32rCanExecutor.ReadTimeoutPropagates
MitsuColtM32rCanExecutor.RefusesATestWritePlanRatherThanWritingForReal
MitsuColtM32rCanExecutor.RejectsAFrameFromTheWrongReplyId
MitsuColtM32rCanExecutor.RejectsAPlanFromAnotherFamilyBeforeAnyIo
MitsuColtM32rCanExecutor.RejectsAResponseToADifferentService
MitsuColtM32rCanExecutor.RejectsInconsistentHandBuiltPlansBeforeAnyIo
MitsuColtM32rCanExecutor.TransportSetupReturnsThePlansWireParameters
MitsuColtM32rCanExecutor.VendorChallengeKeyRejectionStopsBeforeTheSession
MitsuColtM32rCanExecutor.VendorChallengeRejectionStopsBeforeTheSession
MitsuColtM32rCanExecutor.VendorChallengeRunsInBasicSessionBeforeBootloadSession
MitsuColtM32rCanExecutor.WriteBoundsTheDefaultProtocolTo384KiB
MitsuColtM32rCanExecutor.WriteDrivesTheBootloadSessionThenFactorySecurityAccess
MitsuColtM32rCanExecutor.WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo
MitsuColtM32rCanExecutor.WriteRefusesTheBootstrapWhenItsConfirmationIsAbsent
MitsuColtM32rCanExecutor.WriteRefusesTheEraseTriggerWhenItsConfirmationIsAbsent
MitsuColtM32rCanExecutor.WriteSkipsBootstrapWhenTheTopRegionAlreadyMatches
MitsuColtM32rCanExecutor.WriteStopsAtTheNextExchangeWhenCancelledMidWrite
MitsuColtM32rCanExecutor.WriteStopsReadingTheTopRegionAtTheFirstMismatchedChunk
MitsuColtM32rCanExecutor.WriteStopsWhenTheCarrierEraseTriggerReportsANonZeroStatus
MitsuColtM32rCanExecutor/MitsuColtM32rCanExecutorRoutineStatusTest.StopsTheWrite/CrcCheckReportsANonZeroStatus
MitsuColtM32rCanExecutor/MitsuColtM32rCanExecutorRoutineStatusTest.StopsTheWrite/EraseTriggerReportsANonZeroStatus
MitsuColtM32rCanWriteExecutor.BootstrapAbortsWhenTheChecksumRequestDownloadIsRejected
MitsuColtM32rCanWriteExecutor.BootstrapAbortsWhenTheChecksumTransferDataIsRejected
MitsuColtM32rCanWriteExecutor.BootstrapReportsItsOwnReflashUnlockRejection
MitsuColtM32rCanWriteExecutor.WriteAbortsWhenTheEraseRoutineRequestDownloadIsRejected
MitsuColtM32rCanWriteExecutor.WriteAbortsWhenTheWriteRoutineTransferDataIsRejected
MitsuColtM32rCanWriteExecutor.WriteFailsWhenTheTopRegionVerifyMismatches
MitsuColtM32rCanWriteExecutor.WriteFailsWhenTheUserspaceCrcCheckIsRejected
MitsuColtM32rCanWriteExecutor.WriteFailsWhenTheUserspaceVerifyMismatches
MitsuColtM32rCanWriteExecutor.WritePropagatesACarrierReadFailureBeforeRedirectHelpers
MitsuColtM32rCanWriteExecutor.WriteRefusesRedirectBootstrapWhenTheCarrierDoesNotMatchTheDesiredTopPayload
MitsuColtM32rCanWriteExecutor.WriteRunsTheBootstrapWhenTheTopRegionDiffers
MitsuColtM32rCanWriteExecutor.WriteStopsWhenTheEraseTriggerIsRejected
MitsuColtM32rCanWriteExecutor.WriteStopsWhenTheReflashUnlockIsRejected
```

## Boundary preservation

All 47 baseline XML cases retained; candidate has 54.
Direct tests use ExecuteGuarded; original after-write cancellation detail now reads
"CAN read cancelled before delegation". Existing log/detail assertions otherwise remain.
Four explicit Colt polls removed. Mid-read/comparison cancellation now reports the
next address through exchange warnings; accepted erase emits its completion event
before the next request is refused. Invalid-plan and final-result precedence remain.

Task 3 RED: the five specified boundary/conformance tests failed on missing events
or one extra delegated Read; validation and late-completion cases already passed.

### Boundary regressions

| Case | Observable obligation |
| --- | --- |
| PrecancelledExecutionReportsConnectingBeforeRefusal | Connecting info; zero inner Read/Write attempts |
| InvalidPlanStillWinsOverPrecancel | InvalidConfig before any event or I/O |
| ReadStopsAtTheNextChunkWhenCancelledMidRead | Four Read/Write attempts; warning names 0xc0; partial progress below total |
| TopComparisonCancellationIsReportedByTheNextExchange | Four Read/Write attempts; warning names 0x600c0; no upload |
| AcceptedEraseReplyIsReportedBeforeNextRequestCancellation | Accepted erase info; zero attempted userspace RequestDownload |
| FinalReadSuccessSurvivesLateCancellation | Complete 0x60000-byte 5A image; 2051 Read/Write attempts; script consumed |
| FinalVerifyMismatchSurvivesLateCancellation | BadResponse and verify-failed log despite late cancellation; script consumed |
| ColtBoundAttemptStopsBeforeReadWhenCancelledAfterWrite | One accepted Write, zero Read attempts, queued reply retained, one Close |

Cancellation injection is deterministic and asserted in every new cancellation
case; no poll-count trigger, sleep or thread was introduced.

## Named mutation checks

- `runner-bypass`: `src/backend/flash/flash_executor.h`; Read attempts 1 vs 0; exit 3.
- `unlock-abort`: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`; Forbidden request attempts 1 vs 0; exit 3.
- `erase-abort`: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`; Forbidden request attempts 1 vs 0; exit 3.

Full commands and original/mutant SHA-256 values are in the raw evidence manifest.
All mutants restored; all ten focused targets pass afterwards.

## NLOC investment

The candidate source commit is `b9e6e98dedd465a94e381df9e9134f111d012278`.
This ownership change adds support cost; it does not claim test compression or
net reduction. Three C++ files added, five modified, none removed.

| Scope | Baseline | Candidate | Delta |
| --- | ---: | ---: | ---: |
| Whole-tree production | 69539 | 69607 | +68 |
| Whole-tree tests/support | 80111 | 80732 | +621 |
| Whole-tree total | 149650 | 150339 | +689 |
| Changed-scope production | 808 | 876 | +68 |
| Changed-scope test | 1675 | 2296 | +621 |
| Changed-scope total | 2483 | 3172 | +689 |

Per-file values use the union of changed/added/removed C++ paths; absent sides
are zero. Existing nloc_loop.parse_report/classify produce these splits.
Raw hook totals are 149718 and 150407; both include 68 lines outside the metric scope.

## Per-file NLOC

| Changed path | Baseline | Candidate | Delta |
| --- | ---: | ---: | ---: |
| `src/backend/flash/guarded_can_flash_transport.h` | 0 | 20 | +20 |
| `src/backend/flash/guarded_can_flash_transport.cpp` | 0 | 48 | +48 |
| `src/backend/flash/guarded_can_flash_transport_test.cpp` | 0 | 235 | +235 |
| `src/backend/flash/flash_executor.h` | 305 | 319 | +14 |
| `src/backend/flash/bound_flash_attempt_test.cpp` | 307 | 418 | +111 |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h` | 11 | 13 | +2 |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp` | 492 | 476 | -16 |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp` | 1368 | 1643 | +275 |

## Matched runtime comparison

The candidate adds only `//src/backend/flash:guarded_can_flash_transport_test`
to the frozen manifest. Each side has one warm-up and five uncached measured
runs with one local test job and no competing builds. Warm-up logs and all five
measured XML sets are preserved. Sum each root `time` once when present,
otherwise sum direct testsuite children; never add testcase times.

| XML execution seconds | Baseline | Candidate |
| --- | ---: | ---: |
| Sample 1 | 0.026 | 0.029 |
| Sample 2 | 0.026 | 0.028 |
| Sample 3 | 0.026 | 0.029 |
| Sample 4 | 0.025 | 0.029 |
| Sample 5 | 0.026 | 0.028 |
| Median | 0.026 | 0.029 |

`(candidate_median / baseline_median - 1) * 100` is **+11.54%**, below the
25% review threshold. XML timings have millisecond-scale resolution; these
ordinary test runtimes do not predict hardware latency. Build and mutation
times are separate. The initial release application build took 107.521s.

## Automatic mutation limitation and supplemental checks

Changed-line Mull ran all 41 selected binaries with exit 0 but generated no
mutants. A separate non-diff guard-scoped run also generated none. This is a
local tool limitation, not a perfect mutation score or coverage claim. The
initial changed-line run overlapped a supplemental release build sharing
bazel-bin symlinks; discard that run's verdict. The delivery run was repeated
in isolation after source restoration: 1.7s cached build, 128.9s aggregate Mull
execution. The first instrumented build cost 888.2s; its 143.5s overlapping
execution is not acceptance evidence.

Four supplemental guard mutations were rejected by the existing contract tests:

- `write-entry-bypass`: `PrecancelledWriteDoesNotDelegate` failed.
- `write-post-call-check`: `AcceptedWriteSuccessSurvivesInsideCallCancellation` failed.
- `read-entry-bypass`: `PrecancelledReadDoesNotDelegate` failed.
- `read-post-call-check`: `ReadOutcomesPassThrough` failed.

Full commands, source locations, original/mutant SHA-256 values and failing
logs are retained. Every source hash was checked after restoration. Automatic
Mull effectiveness remains unproven locally; no CI policy was changed.

## Gate verdicts

| Gate | Local verdict |
| --- | --- |
| Gazelle generation and dependency review | PASS; guard depends on flash_executor contracts; no concrete guard/family include in runner |
| `prek run --all-files` | PASS |
| Release `//:fastecu` build | PASS |
| Release `//...` tests | PASS; 293 passed, 20 skipped; exact fuzz/platform-incompatible targets in raw log |
| Local macOS packaging | macos_zip built; packaging assemble_test passed through the full release graph |
| `bazel run //:clang_tidy_report_changed` | PASS; seven files, zero standard or inline findings |
| `scripts/coverage-local.sh` | PASS; guard 100% lines/branches; no changed executable production lines uncovered |
| Changed-line Mull command | Exit 0; zero mutants generated; local automatic effectiveness unproven |
| Named source mutation checks | PASS; three planned and four supplemental mutants rejected |
| Matched runtime | PASS; +11.54%, below the 25% threshold |
| NLOC accounting | Complete; +689, no net-reduction claim |
| Linux/macOS/Windows PR, Android portable-core, ASan/UBSan and packaging CI | PASS on the [delivery revision](https://github.com/RcusStackwalker/FastECU/pull/770/checks); changed-line advisory mutation, clang-tidy, Sonar, pre-commit and Gazelle jobs also passed |
| Colt bench checklist | Not performed; qualification unchanged |
| Fresh branch review | PASS; no Critical, Important or Minor findings; delivered in PR #770 |

The guard has 41 contract cases, the runner gains five cases, and Colt gains
seven cases (47 baseline to 54 candidate). No predecessor case was removed.
The only predecessor detail change is the documented after-write cancellation
message. Existing byte/content, timing, error, script and log assertions remain;
the mid-read and two rejection cases gain explicit boundary observations.

## Evidence provenance and document lifecycle

The [raw evidence archive](evidence/colt-cancellation-centralization.tar.gz)
contains logs, five XML sets on each side, coverage reports, per-file NLOC,
command scripts, source hashes and named mutation records. Its SHA256SUMS file
checks every retained member. Generated profiles and binaries are excluded.
The archive's SHA-256 is recorded below after packaging.

The original plan/spec were delivered in PR #770 as
`79cdcfd5f0abfb7bd3317ae91fd793a7fe53b101` and are copied into the evidence.
They were retired in a separate documentation cleanup after preservation and
delivery CI completed. Recover them through
[Git history](../README.md#recover-completed-work-and-deleted-source-citations);
the [flash contract](flash-contracts.md#colt-cancellation-ownership) owns the
implemented boundaries.

At pilot delivery, the independent compression proposal and recorded applicable
Colt bench run were prerequisites for expansion, and no additional family was
enabled. The 2026-10-25 expansion of
[ADR 0025](../adr/0025-enforce-cancellation-in-a-guarded-transport.md#per-executor-migration-requirements)
superseded that blanket prerequisite with per-executor migration requirements.
Both Colt tasks remain outstanding; subsequent adoption is recorded in the
[CAN migration results](can-cancellation-migration-results.md). No hardware
qualification is claimed.

Archive SHA-256: `9d17ae4e9cd517eeca3e7c005cddefd5049a4d7f3292f6a6d980430fb03b9fdb`.

The fresh reviewer independently verified the original archive and all 186 member
checksums, recomputed runtime samples, checked 47 retained predecessor cases and
validated seven mutation restoration hashes. The final review record is retained
in the updated archive; no code fix was required.
