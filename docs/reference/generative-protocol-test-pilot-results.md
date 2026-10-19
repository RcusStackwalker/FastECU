# Generative protocol test pilot results

Work in progress: the pilot is authorized for native execution. No acceptance
or hardware qualification claim is made until the evidence below is complete.
See the [design](../superpowers/specs/2026-10-19-generative-protocol-test-pilot-design.md)
and [implementation plan](../superpowers/plans/2026-10-19-generative-protocol-test-pilot.md).

## Comparison environment

- Source baseline: `b866782bcb6295d4dde8e16eb95567bf7c855e48`.
- Candidate starting commit: `2310ac96c3727f6a869d739117e4964bb8fae6ec`; differs only in pilot design/index documentation.
- Host: Apple Silicon arm64, macOS 27.0.1, Apple clang 21.0.0 (`clang-2100.3.34.2`), Qt 6.11.1.
- Bazel 9.1.1, `--config=release`, repository C++23 flags; runtime scheduling `--jobs=1 --local_test_jobs=1`.
- No PR #756 code or measurements are used.
- Local raw evidence: `/private/tmp/fastecu-generative-pilot-baseline/`. Durable publication location pending final review; local paths alone are not delivered evidence.

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
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.BenchReadReturnsPaddedImage` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.OperatorFacingLogLinesNameThe1_5MFamily` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.InCarReadReturnsPaddedImage` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.ReadDisconnectMidDumpLoopPropagates` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.NegativeResponseDuringConnectFails` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.NegativeResponseAtDumpSetupFails` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.EmptyBranchSelectorReplyFails` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.ObkMismatchAbsentAndEmptyAreMisses` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.ObkErrorsAndCancellationPropagateBeforeInitialization` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.CancellationAfterPageFailsButCancellationAtStopIsSwallowed` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.BenchJumpExhaustionPreservesReadCountAndTiming` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.IdentityExchangeFailuresIncludingCancellationAreNonFatal` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutor.InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutorWrite.WriteErasesThenFlashesBlockOne` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutorWrite.EraseRetryExhaustionFails` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCanExecutorWrite.CancellationAtCloseSettlePreventsChecksum` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.TransportSetupReturnsThePlansWireParameters` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.RejectsAPlanFromAnotherFamilyBeforeAnyIo` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.RefusesATestWritePlanRatherThanWritingForReal` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ReadPropagatesADisconnectedTransport` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ReadStopsAtTheNextChunkWhenCancelledMidRead` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ConnectProbesReadWithThisFamilysProbeTimeout` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ReadReportsAnEmptyReplyAsTimeout` | Retained, migration pending |
| 1_5m: `SubaruDenso1n83m_1_5mCan/CanExecutorConformance/0.ReadTimeoutPropagates` | Retained, migration pending |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/0` | Retained, migration pending |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/1` | Retained, migration pending |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/2` | Retained, migration pending |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/3` | Retained, migration pending |
| 1_5m: `ChecksumOutcomes/SubaruDenso1n83m_1_5mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/4` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ProceedsPastMalformedConnectAndDumpSetupResponses` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ProceedsPastMalformedInCarProbeAndDumpSetup` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.BenchReadReturnsPaddedImage` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.InCarReadReturnsPaddedImage` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ReadDisconnectMidDumpLoopPropagates` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ReadTimeoutAtTheTolerantDumpSetupStillPropagates` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.NegativeResponseDuringConnectFails` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.EmptyBranchSelectorReplyFails` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ObkMismatchAbsentAndEmptyAreMisses` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ObkErrorsAndCancellationPropagateBeforeInitialization` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.CancellationAfterPageFailsButCancellationAtStopIsSwallowed` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.BenchJumpExhaustionPreservesReadCountAndTiming` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.IdentityExchangeFailuresIncludingCancellationAreNonFatal` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutor.InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutorWrite.WriteErasesThenFlashesBlockOne` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutorWrite.EraseRetryExhaustionFails` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCanExecutorWrite.CancellationAtCloseSettlePreventsChecksum` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.TransportSetupReturnsThePlansWireParameters` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.RejectsAPlanFromAnotherFamilyBeforeAnyIo` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.RefusesATestWritePlanRatherThanWritingForReal` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ReadPropagatesADisconnectedTransport` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ReadStopsAtTheNextChunkWhenCancelledMidRead` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ConnectProbesReadWithThisFamilysProbeTimeout` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ReadReportsAnEmptyReplyAsTimeout` | Retained, migration pending |
| 4m: `SubaruDenso1n83m_4mCan/CanExecutorConformance/0.ReadTimeoutPropagates` | Retained, migration pending |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/0` | Retained, migration pending |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/1` | Retained, migration pending |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/2` | Retained, migration pending |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/3` | Retained, migration pending |
| 4m: `ChecksumOutcomes/SubaruDenso1n83m_4mCanExecutorWrite.CloseRetriesSettleAndChecksumOutcomes/4` | Retained, migration pending |

Confirmed **63** executed executor cases in baseline XML.

| Baseline internal branches (both families unless stated) | Protected assertions |
| --- | --- |
| `ObkMismatchAbsentAndEmptyAreMisses` | mismatched positive PDU |
| `ObkMismatchAbsentAndEmptyAreMisses` | no frame |
| `ObkMismatchAbsentAndEmptyAreMisses` | empty PDU: all initialize |
| `ObkErrorsAndCancellationPropagateBeforeInitialization` | disconnected |
| `ObkErrorsAndCancellationPropagateBeforeInitialization` | cancelled: no initialization |
| `ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` | stop succeeds |
| `ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` | stop exhausted: short decrypt, requested-size progress/address, padding, bounded attempts |
| `PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | no frame |
| `PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | empty PDU |
| `PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | cancelled: no stop/progress |
| `CancellationAfterPageFailsButCancellationAtStopIsSwallowed` | cancel after page |
| `CancellationAfterPageFailsButCancellationAtStopIsSwallowed` | cancel at stop: abort versus best-effort success |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | disconnected |
| `IdentityExchangeFailuresIncludingCancellationAreNonFatal` | cancelled outcomes on each of four queries: tolerate, preserve timeout sequence |
| `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | 1.5M outcomes 0/1/2 |
| `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | 4M outcomes 0/1/2/3: acknowledgement/exhaustion/cancellation/additional acknowledgement shape |
| `CloseRetriesSettleAndChecksumOutcomes` | each XML row 0..4: immediate success, pending success, absent, disconnected, pending absent |
| `CloseRetriesSettleAndChecksumOutcomes` | each with six mixed-fault closes |

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

Behavior, size, runtime, mutation comparison, full gates and platform CI are pending.
