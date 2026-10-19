# Colt cancellation centralization results

Status: baseline collection in progress; no implementation or qualification claim.

Implementation base: `883b2afb89464e576ab9a07cdd3442dc997e2130`.
Spec baseline: `2b9613b8275a35fa485caa757a05ba628f163744`.
No drift in Colt implementation or runner header between these revisions.

## Conditions

macOS-27.0.1-arm64-arm-64bit-Mach-O

Apple clang 21.0.0 (clang-2100.3.34.2), arm64; Bazel 9.1.1; pinned Qt 6.8.3.
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

## Retained predecessor cases

```
MitsuColtM32rCanExecutor.RejectsAPlanFromAnotherFamilyBeforeAnyIo
MitsuColtM32rCanExecutor.TransportSetupReturnsThePlansWireParameters
MitsuColtM32rCanExecutor.RejectsInconsistentHandBuiltPlansBeforeAnyIo
MitsuColtM32rCanExecutor.ReadReturnsEachProtocolCapacityFromAddressZero
MitsuColtM32rCanExecutor.ReadReportsAnEmptyReplyAsTimeout
MitsuColtM32rCanExecutor.ReadPropagatesADisconnectedTransport
MitsuColtM32rCanExecutor.ReadTimeoutPropagates
MitsuColtM32rCanExecutor.ReadStopsWhenCancelled
MitsuColtM32rCanExecutor.ReadStopsAtTheNextChunkWhenCancelledMidRead
MitsuColtM32rCanExecutor.VendorChallengeRunsInBasicSessionBeforeBootloadSession
MitsuColtM32rCanExecutor.VendorChallengeRejectionStopsBeforeTheSession
MitsuColtM32rCanExecutor.ReadEmitsMonotonicProgress
MitsuColtM32rCanExecutor.WriteDrivesTheBootloadSessionThenFactorySecurityAccess
MitsuColtM32rCanExecutor.WriteBoundsTheDefaultProtocolTo384KiB
MitsuColtM32rCanExecutor.WriteSkipsBootstrapWhenTheTopRegionAlreadyMatches
MitsuColtM32rCanWriteExecutor.WriteRunsTheBootstrapWhenTheTopRegionDiffers
MitsuColtM32rCanWriteExecutor.WriteRefusesRedirectBootstrapWhenTheCarrierDoesNotMatchTheDesiredTopPayload
MitsuColtM32rCanWriteExecutor.WritePropagatesACarrierReadFailureBeforeRedirectHelpers
MitsuColtM32rCanExecutor.WriteStopsReadingTheTopRegionAtTheFirstMismatchedChunk
MitsuColtM32rCanWriteExecutor.WriteFailsWhenTheUserspaceVerifyMismatches
MitsuColtM32rCanWriteExecutor.WriteFailsWhenTheTopRegionVerifyMismatches
MitsuColtM32rCanWriteExecutor.WriteStopsWhenTheReflashUnlockIsRejected
MitsuColtM32rCanWriteExecutor.WriteStopsWhenTheEraseTriggerIsRejected
MitsuColtM32rCanExecutor.WriteStopsWhenTheCarrierEraseTriggerReportsANonZeroStatus
MitsuColtM32rCanExecutorRoutineStatusTest.StopsTheWrite
MitsuColtM32rCanExecutor.RefusesATestWritePlanRatherThanWritingForReal
MitsuColtM32rCanExecutor.WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo
MitsuColtM32rCanExecutor.WriteRefusesTheBootstrapWhenItsConfirmationIsAbsent
MitsuColtM32rCanExecutor.HandshakeRejectsAReplyTooShortToHoldAServiceByte
MitsuColtM32rCanExecutor.ReadRejectsAChunkAnsweredWithTheWrongService
MitsuColtM32rCanExecutor.WriteStopsAtTheNextExchangeWhenCancelledMidWrite
MitsuColtM32rCanExecutor.ACancelledEraseTriggerIsNotReportedAsAnEcuRejection
MitsuColtM32rCanExecutor.AFailedTransportWriteIsReportedWithoutWaitingForAReply
MitsuColtM32rCanExecutor.ACancellationThatArrivesAfterTheRequestStopsBeforeTheReply
MitsuColtM32rCanWriteExecutor.WriteAbortsWhenTheEraseRoutineRequestDownloadIsRejected
MitsuColtM32rCanWriteExecutor.WriteAbortsWhenTheWriteRoutineTransferDataIsRejected
MitsuColtM32rCanWriteExecutor.WriteFailsWhenTheUserspaceCrcCheckIsRejected
MitsuColtM32rCanWriteExecutor.BootstrapAbortsWhenTheChecksumRequestDownloadIsRejected
MitsuColtM32rCanWriteExecutor.BootstrapAbortsWhenTheChecksumTransferDataIsRejected
MitsuColtM32rCanWriteExecutor.BootstrapReportsItsOwnReflashUnlockRejection
MitsuColtM32rCanExecutor.VendorChallengeKeyRejectionStopsBeforeTheSession
MitsuColtM32rCanExecutor.WriteRefusesTheEraseTriggerWhenItsConfirmationIsAbsent
MitsuColtM32rCanExecutor.AbsorbsResponsePendingWithoutResending
MitsuColtM32rCanExecutor.FailsWhenTheEcuPendsPastTheRepeatLimit
MitsuColtM32rCanExecutor.RejectsAResponseToADifferentService
MitsuColtM32rCanExecutor.RejectsAFrameFromTheWrongReplyId
```

## Baseline measurements

{
  "targets": [
    "//src/backend/flash:flash_executor_test",
    "//src/backend/flash:bound_flash_attempt_test",
    "//src/backend/flash:can_flash_uds_channel_test",
    "//src/backend/flash/ecu:mitsu_colt_m32r_can_plan_test",
    "//src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test",
    "//src/backend/flash/ecu:uds_client_exchange_common_test",
    "//src/backend/flash/testing:scripted_flash_transports_test",
    "//src/backend/protocol/uds:uds_client_test",
    "//src/backend/ports/testing:fake_clock_test"
  ],
  "sums": [
    0.026,
    0.026,
    0.026,
    0.025,
    0.026
  ],
  "median": 0.026
}

NLOC measure: {"total": 149650, "production": 69539, "test": 80111}

Raw NLOC hook total: 149718 (scope classification excludes 68 lines outside the metric).
