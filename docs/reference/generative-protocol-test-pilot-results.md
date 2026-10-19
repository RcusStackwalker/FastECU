# Generative protocol test pilot results

**Outcome: the bounded candidate fails the size and runtime gates.** It adds
837 net NLOC and increases median ordinary execution by 454.7%.
The required behavior matrix and all 20 targeted mutation detectors pass.
This is a failed size-reduction experiment, not a rollout recommendation or a
hardware qualification claim.
See the [design](../superpowers/specs/2026-10-19-generative-protocol-test-pilot-design.md)
and [implementation plan](../superpowers/plans/2026-10-19-generative-protocol-test-pilot.md).

## Comparison environment

- Source baseline: `b866782bcb6295d4dde8e16eb95567bf7c855e48`.
- Candidate starting commit: `2310ac96c3727f6a869d739117e4964bb8fae6ec`; differs only in pilot design/index documentation.
- Host: Apple Silicon arm64, macOS 27.0.1, Apple clang 21.0.0 (`clang-2100.3.34.2`), Qt 6.11.1.
- Bazel 9.1.1, `--config=release`, repository C++23 flags; runtime scheduling `--jobs=1 --local_test_jobs=1`.
- No PR #756 code or measurements are used.
- Delivered [computed evidence](generative-protocol-test-pilot-evidence.json): source IDs, baseline/candidate case manifests, exact mutation patches and failing assertions, runtime samples, per-file NLOC and raw-artifact hashes.
- Raw logs/XML remain in `/private/tmp/fastecu-generative-pilot-baseline/` and `/private/tmp/fastecu-generative-pilot-candidate/`; the delivered JSON records the evidence needed to review and reproduce the comparison without relying on those local paths.

## Baseline

All seven focused targets passed (executor, plan, Denso common, SSM and scripted
transport suites). Baseline suite NLOC: 523 (1.5M), 584 (4M), total 1,107.
Whole-tree NLOC: 149,087, comprising 69,504 production and 79,583 test NLOC.
These are the pinned repository Lizard 1.24.1 hook's measurements.

Runtime targets are the two executor suites and
`//src/backend/flash/testing:scripted_flash_transports_test`.
One warm-up, then five uncached runs; the summed XML execution durations are
0.303s, 0.294s, 0.304s, 0.296s, 0.280s. Median: **0.296s**.

## Assertion replacement ledger

Existing cases stay protected until a replacement is recorded. Internal loop
branches are expanded below rather than inferred from the executed case count.
No assertions have been removed at baseline collection.

| Family / executed baseline case | Candidate / status |
| --- | --- |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.BenchReadReturnsPaddedImage` | `N83m15mRead.ReturnsCompleteIndependentPayload/BenchForward and BenchReverse`: complete independent bytes/padding, operation shape and exact allowed I/O; seed/key timing on bench |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.OperatorFacingLogLinesNameThe1_5MFamily` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.InCarReadReturnsPaddedImage` | `N83m15mRead.ReturnsCompleteIndependentPayload/InCarForward and InCarReverse`: complete independent bytes/padding, operation shape and exact allowed I/O; seed/key timing on bench |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.ReadDisconnectMidDumpLoopPropagates` | `N83m15mTransferFault.AbortsAtSelectedIteration/ReadFirstDisconnected`: disconnected result and exact prefix; no stop or later progress |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.NegativeResponseDuringConnectFails` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.NegativeResponseAtDumpSetupFails` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.EmptyBranchSelectorReplyFails` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.ObkMismatchAbsentAndEmptyAreMisses` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.ObkErrorsAndCancellationPropagateBeforeInitialization` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.CancellationAfterPageFailsButCancellationAtStopIsSwallowed` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.BenchJumpExhaustionPreservesReadCountAndTiming` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.IdentityExchangeFailuresIncludingCancellationAreNonFatal` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutorWrite.WriteErasesThenFlashesBlockOne` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutorWrite.EraseRetryExhaustionFails` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutorWrite.CancellationAtCloseSettlePreventsChecksum` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.TransportSetupReturnsThePlansWireParameters` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.RejectsAPlanFromAnotherFamilyBeforeAnyIo` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.RefusesATestWritePlanRatherThanWritingForReal` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ReadPropagatesADisconnectedTransport` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ReadStopsAtTheNextChunkWhenCancelledMidRead` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ConnectProbesReadWithThisFamilysProbeTimeout` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ReadReportsAnEmptyReplyAsTimeout` | Retained unchanged |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ReadTimeoutPropagates` | Retained unchanged |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/0` | Retained unchanged |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/1` | Retained unchanged |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/2` | Retained unchanged |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/3` | Retained unchanged |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/4` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ProceedsPastMalformedConnectAndDumpSetupResponses` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ProceedsPastMalformedInCarProbeAndDumpSetup` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.BenchReadReturnsPaddedImage` | `N83m4mRead.ReturnsCompleteIndependentPayload/BenchForward and BenchReverse`: complete independent bytes/padding, operation shape and exact allowed I/O; seed/key timing on bench |
| 4m: `SubaruDenso1n83m_4mCanExecutor.InCarReadReturnsPaddedImage` | `N83m4mRead.ReturnsCompleteIndependentPayload/InCarForward and InCarReverse`: complete independent bytes/padding, operation shape and exact allowed I/O; seed/key timing on bench |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ReadDisconnectMidDumpLoopPropagates` | `N83m4mTransferFault.AbortsAtSelectedIteration/ReadFirstDisconnected`: disconnected result and exact prefix; no stop or later progress |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ReadTimeoutAtTheTolerantDumpSetupStillPropagates` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.NegativeResponseDuringConnectFails` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.EmptyBranchSelectorReplyFails` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ObkMismatchAbsentAndEmptyAreMisses` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ObkErrorsAndCancellationPropagateBeforeInitialization` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.CancellationAfterPageFailsButCancellationAtStopIsSwallowed` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.BenchJumpExhaustionPreservesReadCountAndTiming` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.IdentityExchangeFailuresIncludingCancellationAreNonFatal` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutor.InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutorWrite.WriteErasesThenFlashesBlockOne` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutorWrite.EraseRetryExhaustionFails` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCanExecutorWrite.CancellationAtCloseSettlePreventsChecksum` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.TransportSetupReturnsThePlansWireParameters` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.RejectsAPlanFromAnotherFamilyBeforeAnyIo` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.RefusesATestWritePlanRatherThanWritingForReal` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ReadPropagatesADisconnectedTransport` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ReadStopsAtTheNextChunkWhenCancelledMidRead` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ConnectProbesReadWithThisFamilysProbeTimeout` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ReadReportsAnEmptyReplyAsTimeout` | Retained unchanged |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ReadTimeoutPropagates` | Retained unchanged |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/0` | Retained unchanged |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/1` | Retained unchanged |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/2` | Retained unchanged |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/3` | Retained unchanged |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/4` | Retained unchanged |

Confirmed **63** executed executor cases in baseline XML.

| Baseline internal branch (both families unless stated) | Protected assertions; candidate retains the original case |
| --- | --- |
| `ObkMismatchAbsentAndEmptyAreMisses` | mismatched positive PDU: initialize, then propagate the scripted first-page disconnection; initial probe timeout 200 ms |
| `ObkMismatchAbsentAndEmptyAreMisses` | no frame: initialize, then propagate the scripted first-page disconnection; initial probe timeout 200 ms |
| `ObkMismatchAbsentAndEmptyAreMisses` | empty PDU inside a valid envelope: initialize, then propagate the scripted first-page disconnection; initial probe timeout 200 ms |
| `ObkErrorsAndCancellationPropagateBeforeInitialization` | disconnected transport → kDisconnected: do not initialize |
| `ObkErrorsAndCancellationPropagateBeforeInitialization` | cancelled transport → kCancelled: do not initialize |
| `ObkErrorsAndCancellationPropagateBeforeInitialization` | raw empty frame → kBadResponse: do not initialize |
| `ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` | stop acknowledged: short decryption, requested-size address/progress increments, image padding and exact waits |
| `ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` | six stop attempts exhausted: short decryption, requested-size address/progress increments, image padding and exact waits |
| `PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | no frame → kTimeout: no later stop or page progress |
| `PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | raw empty frame → kBadResponse: no later stop or page progress |
| `PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | queued cancellation → kCancelled: no later stop or page progress |
| `CancellationAfterPageFailsButCancellationAtStopIsSwallowed` | after first page: fail, preserving partial progress |
| `CancellationAfterPageFailsButCancellationAtStopIsSwallowed` | at stop log: tolerate cancellation and complete the read |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | ECU ID returns kDisconnected: tolerate; all four failures occur together; next access-method timeout propagates with the original read-timeout sequence |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | VIN returns kDisconnected: tolerate; all four failures occur together; next access-method timeout propagates with the original read-timeout sequence |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | CAL ID returns kDisconnected: tolerate; all four failures occur together; next access-method timeout propagates with the original read-timeout sequence |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | CVN returns kDisconnected: tolerate; all four failures occur together; next access-method timeout propagates with the original read-timeout sequence |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | ECU ID returns kCancelled: tolerate; all four failures occur together; next access-method timeout propagates with the original read-timeout sequence |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | VIN returns kCancelled: tolerate; all four failures occur together; next access-method timeout propagates with the original read-timeout sequence |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | CAL ID returns kCancelled: tolerate; all four failures occur together; next access-method timeout propagates with the original read-timeout sequence |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | CVN returns kCancelled: tolerate; all four failures occur together; next access-method timeout propagates with the original read-timeout sequence |
| `InCarReadReturnsPaddedImage` | 1.5M immediate acknowledgement: baseline asserts complete padded content; the two replacement in-car pattern cases additionally assert the 500 ms settle before the probe |
| `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | 1.5M outcome 1: all eleven replies absent, exhaustion continues; preserve read count, 200 ms timeout policy, log meaning and exact sleep sequence |
| `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | 1.5M outcome 2: queued cancellation propagates; preserve read count, 200 ms timeout policy, log meaning and exact sleep sequence |
| `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | 4M outcome 0: immediate acknowledgement; preserve read count, 200 ms timeout policy, log meaning and exact sleep sequence |
| `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | 4M outcome 1: ten absent reads then unchecked final acknowledgement; preserve read count, 200 ms timeout policy, log meaning and exact sleep sequence |
| `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | 4M outcome 2: queued cancellation propagates; preserve read count, 200 ms timeout policy, log meaning and exact sleep sequence |
| `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | 4M outcome 3: one absent read then acknowledgement; preserve read count, 200 ms timeout policy, log meaning and exact sleep sequence |
| `CloseRetriesSettleAndChecksumOutcomes` | 0: immediate positive checksum; original result and per-family checksum/pending timeouts; settle after close |
| `CloseRetriesSettleAndChecksumOutcomes` | 1: responsePending then success; original result and per-family checksum/pending timeouts; settle after close |
| `CloseRetriesSettleAndChecksumOutcomes` | 2: no frame; original result and per-family checksum/pending timeouts; settle after close |
| `CloseRetriesSettleAndChecksumOutcomes` | 3: disconnected; original result and per-family checksum/pending timeouts; settle after close |
| `CloseRetriesSettleAndChecksumOutcomes` | 4: responsePending then no frame; original result and per-family checksum/pending timeouts; settle after close |
| `CloseRetriesSettleAndChecksumOutcomes` | combined close attempt 0: no frame; six writes/reads, 200 ms read timeouts, no universal stop-on-error rule |
| `CloseRetriesSettleAndChecksumOutcomes` | combined close attempt 1: disconnected; six writes/reads, 200 ms read timeouts, no universal stop-on-error rule |
| `CloseRetriesSettleAndChecksumOutcomes` | combined close attempt 2: negative PDU; six writes/reads, 200 ms read timeouts, no universal stop-on-error rule |
| `CloseRetriesSettleAndChecksumOutcomes` | combined close attempt 3: queued cancellation; six writes/reads, 200 ms read timeouts, no universal stop-on-error rule |
| `CloseRetriesSettleAndChecksumOutcomes` | combined close attempt 4: no frame; six writes/reads, 200 ms read timeouts, no universal stop-on-error rule |
| `CloseRetriesSettleAndChecksumOutcomes` | combined close attempt 5: acknowledged for outcome 0; absent for other outcomes; six writes/reads, 200 ms read timeouts, no universal stop-on-error rule |
| `WriteErasesThenFlashesBlockOne` | first complete literal request: retained unchanged, independent frozen boundary bytes remain executed |
| `WriteErasesThenFlashesBlockOne` | last complete literal request: retained unchanged, independent frozen boundary bytes remain executed |
| `WriteErasesThenFlashesBlockOne` | all intervening requests and image-base subtraction: retained unchanged, independent frozen boundary bytes remain executed |

## Evidence hashes

| Local artifact | SHA-256 |
| --- | --- |
| `focused.log` | `446257eb9b44d8dd81f1e735f501f3362d45d10cecc0e5cb39aad19efc7edc7f` |
| `nloc-files.log` | `3dd5ce246f7629aa77c61a66fc72ba328f91aed0db23ac55a3314a256792b3f3` |
| `nloc-summary.json` | `7ce4d744a84707d6cb9d30ba82a1aa2be6a03484747d30ed0fb557a7a99e91fd` |
| `runtime-0.log` | `e8f4070271620a2d3dcfbdb240ec045dce8ae4983018fd594f562fa2a518b0a7` |
| `runtime-1.log` | `6fe165c021d2b65fc42558a3d953c0984ea318bb86ec05bfa2bc63706f7f4cb3` |
| `runtime-2.log` | `11789737093b6bbda06fc68346ec590d3520fba3ee126385a2f882788e63e0b2` |
| `runtime-3.log` | `1af99d985ffbbc877596199cf63135f8d5658021001b6feefeb80f3ec1932cc7` |
| `runtime-4.log` | `69419b2e4c6494b9f5071aa3333112a06480ed37704f8c2ad2dd65a6eb5a94bb` |
| `runtime-5.log` | `da25707ebfac3e279dc73fbb8992ab9f3551be4cd2cae93e7a013fddf7d56baa` |
| `runtime.json` | `5bb95461e3ba86a4574a596e98c48166788767b69e657d585f4bec8660214178` |
| `scripted_flash_transports_test-0.xml` | `533d559ada8b88d1d96c9e869d0bc411669099cffc5ee99fa0e661267f21f17e` |
| `scripted_flash_transports_test-1.xml` | `0b0a8bfe7379ff0990d13ea27b42a19593da9d0b43f9749d5d026af5cd84dd39` |
| `scripted_flash_transports_test-2.xml` | `5ee368c10ef4b66a26ae2bf94ac1461679218f193be67ab32ecd8147e7fff490` |
| `scripted_flash_transports_test-3.xml` | `7cf09306701b6511a00c35ca874827c951c904e30977123d18531c94464cca23` |
| `scripted_flash_transports_test-4.xml` | `5c41a2fc6c4f5e06230095fa957b9ff5233f1a5db5b396cea86c286498303e27` |
| `scripted_flash_transports_test-5.xml` | `b86e5852d00b3fad88b9ae8334f3fab012291b0fd54c3479428ca31bceac74ac` |
| `subaru_denso_1n83m_1_5m_can_executor_test-0.xml` | `b44a0c246d45056cc6e69718572916328b5e1ebcf23f9db0ed83237222c76010` |
| `subaru_denso_1n83m_1_5m_can_executor_test-1.xml` | `2c76c473afaa32ecfb76825a4b08f080eed7b397892363c5455100d7b43c78c3` |
| `subaru_denso_1n83m_1_5m_can_executor_test-2.xml` | `7bac2571314d14346f0f567db1de9f0900fc19795ee1a8f5d3c66a22f50e8181` |
| `subaru_denso_1n83m_1_5m_can_executor_test-3.xml` | `21140f1c8586a31ad3b45f29264711d0156e911d227de81cd885edf6bb47905e` |
| `subaru_denso_1n83m_1_5m_can_executor_test-4.xml` | `263f50e322dbd56440aa0754d3077813267f8481ae90d4e625b437525998d89c` |
| `subaru_denso_1n83m_1_5m_can_executor_test-5.xml` | `9aa1c970835345fec403dae2b11437dd2935c3bccaa6fdaa73ca3e3429ef63a3` |
| `subaru_denso_1n83m_4m_can_executor_test-0.xml` | `7dd2fe6a52fbcff64d91a5b52f61edc8d7554d5086cc7daa211143ccc50ddae3` |
| `subaru_denso_1n83m_4m_can_executor_test-1.xml` | `ad7646d9d4d57a139d4202f2c22b516a453419664cff26dcd3ff48e807533fcf` |
| `subaru_denso_1n83m_4m_can_executor_test-2.xml` | `fa6625d2673d5f468be400f07d8adf568fceb507eb228d1d453df30ef1e4e754` |
| `subaru_denso_1n83m_4m_can_executor_test-3.xml` | `7c38e8ab3a9b376c6186168fba466aacde4189fa513f8f84c7ada6b9eac25e25` |
| `subaru_denso_1n83m_4m_can_executor_test-4.xml` | `453b203d7299014391ede6a7b5834ad7b8da54261801b7ba67a03b0e2a11d684` |
| `subaru_denso_1n83m_4m_can_executor_test-5.xml` | `50b4dc9f0d240d540f8a8d4cf5e0dc1f6bf337673cc5287932414c8ec8c20774` |

## Candidate verdict

| Gate | Result | Evidence |
| --- | --- | --- |
| Protected obligations plus bounded additions | PASS for this scoped comparison | All original obligations retained or mapped above; both setup sites and both transfer loops exercised; 20/20 candidate mutations detected |
| Remove at least 222 net NLOC including support | FAIL | 1,686 → 2,523: **+837 NLOC** |
| Median ordinary runtime growth at most 25% | FAIL | 0.296s → 1.642s: **+454.7%** |

The bounded migration retains existing shared-helper consumers and original
family-local fault/close cases rather than forcing them into another large
abstraction. Consequently this is a narrower compression attempt than the
plan's intended complete migration. It establishes the cost of the added
coverage and observations; it does not prove that every possible scenario
refactor would fail. No assertion was weakened to meet either numerical gate.
Any smaller useful test improvement needs a separate review; no other family
migration or policy replacement follows from this result.

## Migration scope

The named matrices replace the two uniform-payload successful read cases and
the misleadingly named first-page disconnect case in each family. The baseline
write case still executes the frozen complete first/last requests. Both new
patterns additionally exercise complete writes. All other original cases and
internal branches are retained, including conformance, operator identity,
combined malformed paths, short pages, stop cancellation, jumps, erase and
mixed-fault close/checksum outcomes. No original semantic assertion is dropped.
The old exact uniform fill checks are redundant with stronger full-image
comparisons; no diagnostic narration assertions are removed.


## Candidate size accounting

| File | Baseline NLOC | Candidate NLOC | Delta |
| --- | ---: | ---: | ---: |
| `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp` | 523 | 590 | +67 |
| `src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test.cpp` | 584 | 651 | +67 |
| `src/backend/flash/ecu/testing/n83m_executor_scenarios.h` | 0 | 394 | +394 |
| `src/backend/flash/ecu/testing/n83m_executor_scenarios_test.cpp` | 0 | 64 | +64 |
| `src/backend/flash/ecu/testing/n83m_payloads.h` | 0 | 38 | +38 |
| `src/backend/flash/ecu/testing/n83m_payloads_test.cpp` | 0 | 80 | +80 |
| `src/backend/flash/testing/scripted_can_flash_transport.h` | 203 | 246 | +43 |
| `src/backend/flash/testing/scripted_flash_transports_test.cpp` | 376 | 460 | +84 |

Whole-tree scoped metric: 149,087 → 149,924; production stays
69,504 NLOC (delta zero). The eight-file comparison includes every
new or changed C++ support file and support self-test. No relocation, generated
data exclusion or unrelated cleanup contributes savings.

## Matched runtime

The candidate includes both executor targets, the existing scripted transport
self-test and the two genuinely new support self-test targets. Each run uses
fresh mutable state and shared immutable computed payloads only. The full
focused suite also passes shuffled at GoogleTest seed 431.

| Side | Warm-up (not measured) | Five summed XML execution durations | Median |
| --- | ---: | --- | ---: |
| Baseline | 0.332s | 0.303s, 0.294s, 0.304s, 0.296s, 0.280s | 0.296s |
| Candidate | 1.604s | 1.619s, 1.642s, 1.636s, 1.646s, 1.654s | 1.642s |

Build time and mutation verification are excluded from these execution durations.
The initial focused baseline build took 23.170s; candidate release build and
full-test wall/build timings are recorded separately in the gate logs and are
not a speed comparison. Runtime measurements ran after static analysis finished,
with no competing baseline builds. Case count (63 → 105)
is a completeness cross-check, not an acceptance guard.

## Targeted mutation comparison

All patches were temporary; production files match the pinned source after
restoration. A named assertion failure is required for detection. The JSON
contains exact patches and assertion text, including detector case names.
Failure line numbers reflect the test revision used during implementation; the
final restored C++ source hashes identify the ordinary-test/runtime snapshot.

| Deliberate defect | Baseline | Candidate |
| --- | --- | --- |
| `strict-download` | `SubaruDenso1n83m_1_5mCanExecutor.NegativeResponseAtDumpSetupFails` | `SitesAndReplies/N83m15mSetup.PreservesSitePolicy/DownloadMismatch` |
| `strict-upload` | `survived: pre-existing gap` | `SitesAndReplies/N83m15mSetup.PreservesSitePolicy/UploadMismatch` |
| `tolerant-setup` | `SubaruDenso1n83m_4mCanExecutor.ProceedsPastMalformedConnectAndDumpSetupResponses` | `SitesAndReplies/N83m4mSetup.PreservesSitePolicy/DownloadNoFrame; SitesAndReplies/N83m4mSetup.PreservesSitePolicy/UploadNoFrame` |
| `ignore-fatal-page` | `SubaruDenso1n83m_1_5mCanExecutor.ReadDisconnectMidDumpLoopPropagates` | `OperationsAndBoundaries/N83m15mTransferFault.AbortsAtSelectedIteration/ReadFirstDisconnected` |
| `swallowed-extra-write` | `survived: pre-existing gap` | `PathsAndPatterns/N83m15mRead.ReturnsCompleteIndependentPayload/BenchForward` |
| `swallowed-extra-read` | `survived: pre-existing gap` | `PathsAndPatterns/N83m15mRead.ReturnsCompleteIndependentPayload/BenchForward` |
| `image-base-shift` | `SubaruDenso1n83m_1_5mCanExecutorWrite.WriteErasesThenFlashesBlockOne` | `Patterns/N83m15mWritePattern.SendsCompleteIndependentPayload/Forward` |
| `omit-page-content` | `SubaruDenso1n83m_1_5mCanExecutor.BenchReadReturnsPaddedImage` | `PathsAndPatterns/N83m15mRead.ReturnsCompleteIndependentPayload/BenchForward` |
| `reorder-page-address` | `SubaruDenso1n83m_1_5mCanExecutor.BenchReadReturnsPaddedImage` | `PathsAndPatterns/N83m15mRead.ReturnsCompleteIndependentPayload/BenchForward` |
| `truncate-read` | `SubaruDenso1n83m_1_5mCanExecutor.BenchReadReturnsPaddedImage` | `PathsAndPatterns/N83m15mRead.ReturnsCompleteIndependentPayload/BenchForward` |
| `truncate-write` | `SubaruDenso1n83m_1_5mCanExecutorWrite.WriteErasesThenFlashesBlockOne` | `Patterns/N83m15mWritePattern.SendsCompleteIndependentPayload/Forward` |
| `bench-first-1_5m` | `SubaruDenso1n83m_1_5mCanExecutor.BenchReadReturnsPaddedImage` | `PathsAndPatterns/N83m15mRead.ReturnsCompleteIndependentPayload/BenchForward` |
| `checksum-timeout-1_5m` | `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/1` | `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/1` |
| `bench-first-4m` | `SubaruDenso1n83m_4mCanExecutor.BenchReadReturnsPaddedImage` | `PathsAndPatterns/N83m4mRead.ReturnsCompleteIndependentPayload/BenchForward` |
| `read-cancellation-boundary` | `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ReadStopsAtTheNextChunkWhenCancelledMidRead` | `OperationsAndBoundaries/N83m15mTransferFault.AbortsAtSelectedIteration/ReadMiddleCancelled` |
| `write-cancellation-boundary` | `survived: pre-existing gap` | `OperationsAndBoundaries/N83m15mTransferFault.AbortsAtSelectedIteration/WriteMiddleCancelled` |
| `checksum-timeout-4m` | `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/1` | `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/1` |
| `in-car-settle-position-1_5m` | `survived: pre-existing gap` | `PathsAndPatterns/N83m15mRead.ReturnsCompleteIndependentPayload/InCarForward` |
| `in-car-settle-position-4m` | `survived: pre-existing gap` | `PathsAndPatterns/N83m4mRead.ReturnsCompleteIndependentPayload/InCarForward` |
| `image-base-full-offset` | `SubaruDenso1n83m_1_5mCanExecutorWrite.WriteErasesThenFlashesBlockOne` | `Patterns/N83m15mWritePattern.SendsCompleteIndependentPayload/Forward` |

The baseline detects 14/20; the candidate detects 20/20. The gaps exposed are
strict upload rejection, swallowed extra writes/reads, middle-write
cancellation, and misplaced in-car settles in both families. The first attempted 4M checksum patch failed compilation because
it made a constant unused; it was replaced by a compilable timeout-value patch
and is not counted as a detector. The post-admission TestWrite guard remains
unreachable independently of admission rejection; no suppression or artificial
production route was introduced to mutate it.

## Verification and limits

- Release application build: PASS.
- Full release graph: 293 targets pass; 14 platform/fuzz targets are skipped by host selection. The baseline reverse-dependency manifest contains 40 passing consumers; the candidate full graph includes them.
- All nine focused targets pass; shuffled focused checks pass at seed 431.
- Formatting/complexity hooks, Gazelle regeneration/check and Markdown links: PASS.
- Changed-code clang-tidy report: PASS after mechanical brace/constant-naming corrections. The runner reports no co-located source for the fake header; its changed test translation unit and pilot includers are among the five analyzed units. This is not a claim of universal header-includer analysis.
- Platform CI/packaging: pending; no PR or push was authorized in this session.
- Bench/in-car hardware qualification: unchanged; all new execution is scripted software testing.

The payload dictionary checks mapping and orchestration, not arbitrary cipher
inputs. Existing algorithm/plan tests remain evidence dependencies. Observation
coverage applies to base fake calls; a subclass bypassing them is outside it.
The original mixed-fault close and identity semantics remain family-local and
unchanged. The size/runtime failures prohibit describing this as a successful
pilot or extrapolating savings to other families.

Bazel elapsed time for the 20 final valid mutation invocations, including builds,
was 52.322s on baseline and 63.06s on candidate. These are verification costs, not ordinary test runtime.

## Final review disposition

One fresh whole-branch reviewer checked the candidate, independently confirmed
runtime medians, and checked mutation patch hashes. Its Important finding was
missing read sleep/I/O-position assertions. The single fix pass now snapshots
both read paths as well as writes. Moving the in-car 500 ms settle past its
probe survived before the fix and fails afterward for both families; restored
executors pass. The additional full image-origin `0x10000` shift also fails a
named independent-payload assertion. Numerical measurements above were refreshed
after this pass.

Execution rulings and their limits:

- Retain other original obligations in place: existing helpers already share them; the cost is a narrower compression attempt and less potential savings.
- Correct the immediate-ack ledger mapping to the executing read-success case: crediting the nonexistent 1.5M outcome-0 branch would misstate evidence. This documentation accuracy issue was treated as part of the required fix pass.
- The tolerant-setup mutation already detects Download and Upload; both case names are now shown rather than only the first JSON failure. Treating the first summary name as exhaustive would understate the existing evidence.
- Hardware qualification/physical timing remain unchanged: scripted checks do not remove the risk of unqualified hardware behavior.
- Platform CI/packaging remain pending: host-only success does not remove platform risk, and no push was authorized.
- The negative result applies only to this bounded candidate: a better abstraction remains unexplored.
- Arbitrary cipher inputs remain owned by the unchanged algorithm suites: the dictionary is a mapping oracle, not comprehensive crypto evidence.
- Concurrent fake I/O and subclasses bypassing its base methods stay outside the observation contract: those uses need separate evidence.

No further reviewer was dispatched and no unrelated polishing was folded into
the fix pass. No remaining Critical/Important review finding is knowingly left
open; external platform gates remain pending.
