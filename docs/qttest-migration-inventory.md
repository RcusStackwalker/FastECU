# QtTest migration coverage inventory

Baseline: `67643874`. All 44 QtTest executables retain their cases. Names in the replacement column are GoogleTest case names; named parameter rows replace punctuation with underscores. The two GoogleTest files that mentioned QtTest entry-point macros only in comments are not counted.


Inventory: **44 executables, 467 baseline case methods** (parameter rows expand these methods).


## apps/desktop/desktop_composition_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `compositionRegistersAllLoggingProtocolsWithoutWindow` | `DesktopCompositionTest.compositionRegistersAllLoggingProtocolsWithoutWindow` |
| `servicesReferToTheCompositionsOwnObjects` | `DesktopCompositionTest.servicesReferToTheCompositionsOwnObjects` |
| `failedStartupBuildsNoServicesAndPerformsNoEcuIo` | `DesktopCompositionTest.failedStartupBuildsNoServicesAndPerformsNoEcuIo` |
| `workspaceOpensARomFromDisk` | `DesktopCompositionTest.workspaceOpensARomFromDisk` |
| `migrationLoadsPreviousVersionSettingsFromDisk` | `DesktopCompositionTest.migrationLoadsPreviousVersionSettingsFromDisk` |
| `malformedSettingsRejectStartup` | `DesktopCompositionTest.malformedSettingsRejectStartup` |
| `settingsRewriteFailureIsAStartupWarning` | `DesktopCompositionTest.settingsRewriteFailureIsAStartupWarning` |
| `servicesShareTheCompositionsSession` | `DesktopCompositionTest.servicesShareTheCompositionsSession` |
| `restartSeesSavedSettingsButNotTheDatalogDirectory` | `DesktopCompositionTest.restartSeesSavedSettingsButNotTheDatalogDirectory` |
| `waitRequestIsWiredToTheRemoteUtility` | `DesktopCompositionTest.waitRequestIsWiredToTheRemoteUtility` |
| `remoteStateChangesReachThePeer` | `DesktopCompositionTest.remoteStateChangesReachThePeer` |
| `mirroringWithoutAPeerReturnsPromptly` | `DesktopCompositionTest.mirroringWithoutAPeerReturnsPromptly` |
| `channelLevelsReachTheLogWindowWithTheirPrefix` | `DesktopCompositionTest.channelLevelsReachTheLogWindowWithTheirPrefix` |
| `debugLinesStayOutOfTheLogWindow` | `DesktopCompositionTest.debugLinesStayOutOfTheLogWindow` |
| `relayedLineSurvivesItsSenderButADirectOneDoesNot` | `DesktopCompositionTest.relayedLineSurvivesItsSenderButADirectOneDoesNot` |
| `enablingFileLoggingWritesASyslogFile` | `DesktopCompositionTest.enablingFileLoggingWritesASyslogFile` |
| `destructionRightAfterConstructionDoesNotHang` | `DesktopCompositionTest.destructionRightAfterConstructionDoesNotHang` |
| `constructingTwiceInOneProcessSucceeds` | `DesktopCompositionTest.constructingTwiceInOneProcessSucceeds` |
| `emptyHostSelectsTheDirectBackend` | `DesktopCompositionTest.emptyHostSelectsTheDirectBackend` |
| `nonEmptyHostSelectsTheRemoteBackendWithItsCredentials` | `DesktopCompositionTest.nonEmptyHostSelectsTheRemoteBackendWithItsCredentials` |

Conditional skip sites: 1 → 1.

Application: Core.

## apps/desktop/startup_diagnostics_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `realPresentersReportSeverityAndOrderedDetails` | `StartupDiagnosticsTest.realPresentersReportSeverityAndOrderedDetails` |
| `emptyWarningsDoNotOpenAModalOrLog` | `StartupDiagnosticsTest.emptyWarningsDoNotOpenAModalOrLog` |
| `sinkRetainsBoundedUtf8DiagnosticsInOrder` | `StartupDiagnosticsTest.sinkRetainsBoundedUtf8DiagnosticsInOrder` |
| `failureTextCarriesTheDetail` | `StartupDiagnosticsTest.failureTextCarriesTheDetail` |
| `warningTextListsEveryWarning` | `StartupDiagnosticsTest.warningTextListsEveryWarning` |
| `defaultRootIsUnderHomeAndEndsInFastEcu` | `StartupDiagnosticsTest.defaultRootIsUnderHomeAndEndsInFastEcu` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/platform/desktop/common/connection/adapter_connection_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `parsesTheToolbarTransportText` | `TestAdapterConnection.parsesTheToolbarTransportText` |
| `listsPortsFromTheFacade` | `TestAdapterConnection.listsPortsFromTheFacade` |
| `setInitialPortSetsTheBaudThenThePort` | `TestAdapterConnection.setInitialPortSetsTheBaudThenThePort` |
| `selectPortReplacesThePortList` | `TestAdapterConnection.selectPortReplacesThePortList` |
| `openAndIsOpenAskTheFacade` | `TestAdapterConnection.openAndIsOpenAskTheFacade` |
| `openedPortAsksTheFacade` | `TestAdapterConnection.openedPortAsksTheFacade` |
| `canTransportIsRawCanElevenBit` | `TestAdapterConnection.canTransportIsRawCanElevenBit` |
| `iso15765TransportIsTwentyNineBit` | `TestAdapterConnection.iso15765TransportIsTwentyNineBit` |
| `klineWithSsmRunsAtFourThousandEightHundred` | `TestAdapterConnection.klineWithSsmRunsAtFourThousandEightHundred` |
| `klineWithoutSsmLeavesTheSpeedAlone` | `TestAdapterConnection.klineWithoutSsmLeavesTheSpeedAlone` |
| `clearLinkFlagsClearsEveryFlagAndKeepsParity` | `TestAdapterConnection.clearLinkFlagsClearsEveryFlagAndKeepsParity` |
| `returnToIdleResetsBaudAndParityAndKeepsTheFlags` | `TestAdapterConnection.returnToIdleResetsBaudAndParityAndKeepsTheFlags` |
| `setPortSpeedPassesTheBaudAsText` | `TestAdapterConnection.setPortSpeedPassesTheBaudAsText` |
| `batteryIsReadOnlyFromAnOpenPort` | `TestAdapterConnection.batteryIsReadOnlyFromAnOpenPort` |
| `waitForSourceWaitsOnTheFacade` | `TestAdapterConnection.waitForSourceWaitsOnTheFacade` |
| `forwardsFacadeStateChanges` | `TestAdapterConnection.forwardsFacadeStateChanges` |
| `exposesTheSameFacade` | `TestAdapterConnection.exposesTheSameFacade` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/diagnostics/dtc_worker_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `reportsTheSessionOutcomeAndForwardsLogLines` | `DtcWorkerTest.reportsTheSessionOutcomeAndForwardsLogLines` |
| `stopBeforeStartCancelsTheRun` | `DtcWorkerTest.stopBeforeStartCancelsTheRun` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/platform/desktop/common/diagnostics/serial_diagnostic_link_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `klineOpenResetsAppliesEverySetterThenOpens` | `TestSerialDiagnosticLink.klineOpenResetsAppliesEverySetterThenOpens` |
| `canOpenResetsAppliesEverySetterThenOpens` | `TestSerialDiagnosticLink.canOpenResetsAppliesEverySetterThenOpens` |
| `evenParityIsAppliedBeforeTheOpen` | `TestSerialDiagnosticLink.evenParityIsAppliedBeforeTheOpen` |
| `failingSetterIsInvalidConfigAndStopsTheSequence` | `TestSerialDiagnosticLink.failingSetterIsInvalidConfigAndStopsTheSequence` |
| `emptyOpenedPortIsDisconnected` | `TestSerialDiagnosticLink.emptyOpenedPortIsDisconnected` |
| `setHeaderSetsAllThreeFlags` | `TestSerialDiagnosticLink.setHeaderSetsAllThreeFlags` |
| `p1UsesTheJ2534IoctlOnOpenPort` | `TestSerialDiagnosticLink.p1UsesTheJ2534IoctlOnOpenPort` |
| `p1UsesKlineTimingsOnDirectSerial` | `TestSerialDiagnosticLink.p1UsesKlineTimingsOnDirectSerial` |
| `initCallsPassBytesThrough` | `TestSerialDiagnosticLink.initCallsPassBytesThrough` |
| `writeIsEchoCheckedAndReadsSelectTheFacadeCall` | `TestSerialDiagnosticLink.writeIsEchoCheckedAndReadsSelectTheFacadeCall` |
| `cancelledReadNeverReachesTheFacade` | `TestSerialDiagnosticLink.cancelledReadNeverReachesTheFacade` |
| `nullFacadeIsDisconnected` | `TestSerialDiagnosticLink.nullFacadeIsDisconnected` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/diagnostics/ssm_identify_worker_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `stopsAtTheFirstSuccess` | `SsmIdentifyWorkerTest.stopsAtTheFirstSuccess` |
| `retriesFiveTimesThenReportsTheLastError` | `SsmIdentifyWorkerTest.retriesFiveTimesThenReportsTheLastError` |
| `stopBeforeStartCancelsAfterOneAttempt` | `SsmIdentifyWorkerTest.stopBeforeStartCancelsAfterOneAttempt` |
| `destroyingARunningWorkerJoinsIt` | `SsmIdentifyWorkerTest.destroyingARunningWorkerJoinsIt` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/platform/desktop/common/flash/flash_worker_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `closingWhileReadIsBlocked_cancelsUnblocksAndJoinsWithoutWallClockSleep` | `TestFlashWorker.closingWhileReadIsBlocked_cancelsUnblocksAndJoinsWithoutWallClockSleep` |
| `oneAndOnlyOneTerminalResultIsEmitted` | `TestFlashWorker.oneAndOnlyOneTerminalResultIsEmitted` |
| `phaseProgressIsForwardedAlongsideLegacyProgress` | `TestFlashWorker.phaseProgressIsForwardedAlongsideLegacyProgress` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/flash/flash_workflow_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone` | `FlashWorkflowTest.recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone` |
| `invalidColtSuffixIsRecognizedButFailsPreflight` | `FlashWorkflowTest.invalidColtSuffixIsRecognizedButFailsPreflight` |
| `preflightPrecedesPromptsAndDeclineCancels` | `FlashWorkflowTest.preflightPrecedesPromptsAndDeclineCancels` |
| `successfulReadBytesAreAcceptedAutomatically` | `FlashWorkflowTest.successfulReadBytesAreAcceptedAutomatically` |
| `subaruMitsuPropagatesRomId` | `FlashWorkflowTest.subaruMitsuPropagatesRomId` |
| `subaruHitachiRoutesBothModesAndPropagatesReadResult` | `FlashWorkflowTest.subaruHitachiRoutesBothModesAndPropagatesReadResult` |
| `routesTcuHitachiM32rKlineReadOnly` | `FlashWorkflowTest.routesTcuHitachiM32rKlineReadOnly` |
| `unisiaJecsRoutesOnlyExactProtocolMcuPairs` | `FlashWorkflowTest.unisiaJecsRoutesOnlyExactProtocolMcuPairs` |
| `unisiaJecsCrossPairsFailBeforeAttempt` | `FlashWorkflowTest.unisiaJecsCrossPairsFailBeforeAttempt` |
| `routesTcuHitachiM32rCanReadAndWriteRejectsTestWrite` | `FlashWorkflowTest.routesTcuHitachiM32rCanReadAndWriteRejectsTestWrite` |
| `routesSh72543rAliasesAndPreservesImageAndIdentity` | `FlashWorkflowTest.routesSh72543rAliasesAndPreservesImageAndIdentity` |
| `routesSh7058ReadAndWriteWithPreTransportPrompts` | `FlashWorkflowTest.routesSh7058ReadAndWriteWithPreTransportPrompts` |
| `sh72543rRejectsPreflightAndDeclinedBegin` | `FlashWorkflowTest.sh72543rRejectsPreflightAndDeclinedBegin` |
| `sh72543rPropagatesFailureAndAbsentIdentity` | `FlashWorkflowTest.sh72543rPropagatesFailureAndAbsentIdentity` |
| `coltWriteUsesColtSpecificSafetyPrompts` | `FlashWorkflowTest.coltWriteUsesColtSpecificSafetyPrompts` |
| `mc68BdmReadRoutesThroughBeginToAttempt` | `FlashWorkflowTest.mc68BdmReadRoutesThroughBeginToAttempt` |
| `mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom` | `FlashWorkflowTest.mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom` |
| `mc68BdmDeclinedBootstrapConfirmationCancels` | `FlashWorkflowTest.mc68BdmDeclinedBootstrapConfirmationCancels` |
| `mc68BdmDeclinedBeginCancels` | `FlashWorkflowTest.mc68BdmDeclinedBeginCancels` |
| `mc68BdmTestWriteFailsBeforeAnyPrompt` | `FlashWorkflowTest.mc68BdmTestWriteFailsBeforeAnyPrompt` |
| `mc68BdmPrefixLookalikeStaysOffTheKlineFamily` | `FlashWorkflowTest.mc68BdmPrefixLookalikeStaysOffTheKlineFamily` |
| `mc68TpuProtocolIsClaimedByPortableRoute` | `FlashWorkflowTest.mc68TpuProtocolIsClaimedByPortableRoute` |
| `mc68Revision04IsClaimedButPlanBuildFails` | `FlashWorkflowTest.mc68Revision04IsClaimedButPlanBuildFails` |
| `sh7055ProtocolIsClaimedByPortableRoute` | `FlashWorkflowTest.sh7055ProtocolIsClaimedByPortableRoute` |
| `densoCanRoutesOnlyTheFiveExactProtocols` | `FlashWorkflowTest.densoCanRoutesOnlyTheFiveExactProtocols` |
| `densoCanResolvesKernelPromptsAndPropagatesAttemptResult` | `FlashWorkflowTest.densoCanResolvesKernelPromptsAndPropagatesAttemptResult` |
| `densoCanPreflightAndDeclinedPromptsStopBeforeAttempt` | `FlashWorkflowTest.densoCanPreflightAndDeclinedPromptsStopBeforeAttempt` |
| `petrolRoutesOnlyTheFiveExactProtocols` | `FlashWorkflowTest.petrolRoutesOnlyTheFiveExactProtocols` |
| `petrolSupportedOperationsResolveSecurityAndCatalogKernel` | `FlashWorkflowTest.petrolSupportedOperationsResolveSecurityAndCatalogKernel` |
| `petrolSuccessfulReadPropagatesBytesAndRomId` | `FlashWorkflowTest.petrolSuccessfulReadPropagatesBytesAndRomId` |
| `petrolReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport` | `FlashWorkflowTest.petrolReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport` |
| `dieselRoutesOnlyTheTwoExactProtocols` | `FlashWorkflowTest.dieselRoutesOnlyTheTwoExactProtocols` |
| `dieselSupportedOperationsResolveGenerationCatalogKernels` | `FlashWorkflowTest.dieselSupportedOperationsResolveGenerationCatalogKernels` |
| `dieselSuccessfulReadPropagatesKernelSnapshotBytesAndRomId` | `FlashWorkflowTest.dieselSuccessfulReadPropagatesKernelSnapshotBytesAndRomId` |
| `dieselReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport` | `FlashWorkflowTest.dieselReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport` |
| `tcuRoutesOnlyTheTwoExactProtocols` | `FlashWorkflowTest.tcuRoutesOnlyTheTwoExactProtocols` |
| `tcuSupportedOperationsResolveTheirCatalogKernelAndReachAttempt` | `FlashWorkflowTest.tcuSupportedOperationsResolveTheirCatalogKernelAndReachAttempt` |
| `tcuUnsupportedOperationsFailBeforeTransportIo` | `FlashWorkflowTest.tcuUnsupportedOperationsFailBeforeTransportIo` |
| `tcuReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport` | `FlashWorkflowTest.tcuReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport` |
| `tcuSuccessfulReadPropagatesBytesAndRomId` | `FlashWorkflowTest.tcuSuccessfulReadPropagatesBytesAndRomId` |
| `mc68ResolvesKernelThroughCatalogBeforePromptAndAttempt` | `FlashWorkflowTest.mc68ResolvesKernelThroughCatalogBeforePromptAndAttempt` |
| `missingCatalogKernelFailsBeforePrompt` | `FlashWorkflowTest.missingCatalogKernelFailsBeforePrompt` |
| `sh7055IteratesConfirmationsAndPropagatesAttemptResult` | `FlashWorkflowTest.sh7055IteratesConfirmationsAndPropagatesAttemptResult` |
| `sh7055EcutekResolvesWithoutCarModelReference` | `FlashWorkflowTest.sh7055EcutekResolvesWithoutCarModelReference` |
| `portableImageCopiesRomForEveryNonReadOperation` | `FlashWorkflowTest.portableImageCopiesRomForEveryNonReadOperation` |
| `mc68TestWriteWithPortableImageReachesAttempt` | `FlashWorkflowTest.mc68TestWriteWithPortableImageReachesAttempt` |
| `mc68PhysicalImageIsPackedAtWorkflowBoundary` | `FlashWorkflowTest.mc68PhysicalImageIsPackedAtWorkflowBoundary` |
| `mc68CalibrationPaddingRoundTripsToPackedWriteImage` | `FlashWorkflowTest.mc68CalibrationPaddingRoundTripsToPackedWriteImage` |
| `sh7055TestWriteWithPortableImageReachesPromptsAndAttempt` | `FlashWorkflowTest.sh7055TestWriteWithPortableImageReachesPromptsAndAttempt` |
| `mc68TpuReadResolvesCatalogAndReachesAttempt` | `FlashWorkflowTest.mc68TpuReadResolvesCatalogAndReachesAttempt` |
| `densoSh705xKlineRoutesExactProtocolsThroughBeginToAttempt` | `FlashWorkflowTest.densoSh705xKlineRoutesExactProtocolsThroughBeginToAttempt` |
| `densoSh705xKlineCobbReadFailsBeforeAttempt` | `FlashWorkflowTest.densoSh705xKlineCobbReadFailsBeforeAttempt` |
| `densoSh705xKlineIgnoresPrefixLookalikes` | `FlashWorkflowTest.densoSh705xKlineIgnoresPrefixLookalikes` |
| `unisiaJecsM32rRoutesTheFourExactProtocols` | `FlashWorkflowTest.unisiaJecsM32rRoutesTheFourExactProtocols` |
| `unisiaJecsM32rLookalikesStayUnrouted` | `FlashWorkflowTest.unisiaJecsM32rLookalikesStayUnrouted` |
| `unisiaBootmodeReadUsesTheKlineReadFamily` | `FlashWorkflowTest.unisiaBootmodeReadUsesTheKlineReadFamily` |
| `unisiaBootmodeWriteRunsKernelThenMod1ThenProgram` | `FlashWorkflowTest.unisiaBootmodeWriteRunsKernelThenMod1ThenProgram` |
| `unisiaBootmodeKernelFailureSkipsMod1AndProgram` | `FlashWorkflowTest.unisiaBootmodeKernelFailureSkipsMod1AndProgram` |
| `unisiaBootmodeKernelCancelledShowsNotice` | `FlashWorkflowTest.unisiaBootmodeKernelCancelledShowsNotice` |
| `unisiaBootmodeDeclinedMod1CancelsWithNotice` | `FlashWorkflowTest.unisiaBootmodeDeclinedMod1CancelsWithNotice` |
| `unisiaBootmodeProgramFailureShowsNoticeThenFailure` | `FlashWorkflowTest.unisiaBootmodeProgramFailureShowsNoticeThenFailure` |
| `unisiaBootmodeDeclinedVoltagesCancelsBeforeAnyAttempt` | `FlashWorkflowTest.unisiaBootmodeDeclinedVoltagesCancelsBeforeAnyAttempt` |
| `unisiaBootmodeWrongImageSizeFailsBeforeAnyPrompt` | `FlashWorkflowTest.unisiaBootmodeWrongImageSizeFailsBeforeAnyPrompt` |
| `unisiaBootmodeMissingKernelFailsBeforeAnyPrompt` | `FlashWorkflowTest.unisiaBootmodeMissingKernelFailsBeforeAnyPrompt` |
| `unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt` | `FlashWorkflowTest.unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt` |
| `unisiaBootmodeTestWriteIsUnsupported` | `FlashWorkflowTest.unisiaBootmodeTestWriteIsUnsupported` |
| `unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter` | `FlashWorkflowTest.unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter` |
| `unisiaJecsM32rFailedWriteRemindsBeforeReportingTheFailure` | `FlashWorkflowTest.unisiaJecsM32rFailedWriteRemindsBeforeReportingTheFailure` |
| `unisiaJecsM32rCancelledWriteReminds` | `FlashWorkflowTest.unisiaJecsM32rCancelledWriteReminds` |
| `unisiaJecsM32rDeclinedVppPromptCancelsBeforeAttempt` | `FlashWorkflowTest.unisiaJecsM32rDeclinedVppPromptCancelsBeforeAttempt` |
| `unisiaJecsM32rAdapterSuppliedVppSkipsBothPrompts` | `FlashWorkflowTest.unisiaJecsM32rAdapterSuppliedVppSkipsBothPrompts` |
| `unisiaJecsM32rAdapterSuppliedVppFailedWriteWarnsNotToPowerOff` | `FlashWorkflowTest.unisiaJecsM32rAdapterSuppliedVppFailedWriteWarnsNotToPowerOff` |
| `unisiaJecsM32rAdapterSuppliedVppCancelledWriteWarnsNotToPowerOff` | `FlashWorkflowTest.unisiaJecsM32rAdapterSuppliedVppCancelledWriteWarnsNotToPowerOff` |
| `unisiaJecsM32rReadPropagatesRomIdWithoutVppPrompts` | `FlashWorkflowTest.unisiaJecsM32rReadPropagatesRomIdWithoutVppPrompts` |
| `unisiaJecsM32rFailedReadReportsWithoutNotice` | `FlashWorkflowTest.unisiaJecsM32rFailedReadReportsWithoutNotice` |
| `unisiaJecsM32rWriteOnReadOnlyVariantFailsBeforeAnyPrompt` | `FlashWorkflowTest.unisiaJecsM32rWriteOnReadOnlyVariantFailsBeforeAnyPrompt` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/platform/desktop/common/logging/logging_engine_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `start_rejections` | `start_rejectionsParameters.start_rejections` |
| `user_stop_publishes_joined_completion_exactly_once` | `TestLoggingEngine.user_stop_publishes_joined_completion_exactly_once` |
| `completion_observer_can_immediately_start_a_second_run` | `TestLoggingEngine.completion_observer_can_immediately_start_a_second_run` |
| `explicit_stop_restart_ignores_stale_worker_events_and_preserves_handshake_classification` | `TestLoggingEngine.explicit_stop_restart_ignores_stale_worker_events_and_preserves_handshake_classification` |
| `natural_terminal_result_is_published_once_after_reprocessing_queued_delivery` | `TestLoggingEngine.natural_terminal_result_is_published_once_after_reprocessing_queued_delivery` |
| `successful_worker_result_is_reported_as_runtime_failure` | `TestLoggingEngine.successful_worker_result_is_reported_as_runtime_failure` |
| `destruction_joins_blocked_run_without_publishing_completion` | `TestLoggingEngine.destruction_joins_blocked_run_without_publishing_completion` |
| `every_cdbg_serial_setup_failure_is_structured_and_stops_before_later_steps` | `TestLoggingEngine.every_cdbg_serial_setup_failure_is_structured_and_stops_before_later_steps` |
| `start_error_preserves_handshake_failure_ui_path` | `TestLoggingEngine.start_error_preserves_handshake_failure_ui_path` |
| `disconnect_error_preserves_adapter_failure_ui_path` | `TestLoggingEngine.disconnect_error_preserves_adapter_failure_ui_path` |
| `post_start_failure_is_not_reported_as_handshake_failure` | `TestLoggingEngine.post_start_failure_is_not_reported_as_handshake_failure` |
| `unexpected_cancelled_outcome_is_reported_as_runtime_failure` | `TestLoggingEngine.unexpected_cancelled_outcome_is_reported_as_runtime_failure` |
| `diagnostic_slot_forwards_error_level_with_timestamp_and_linefeed` | `TestLoggingEngine.diagnostic_slot_forwards_error_level_with_timestamp_and_linefeed` |
| `diagnostic_slot_forwards_warning_level_with_timestamp_and_linefeed` | `TestLoggingEngine.diagnostic_slot_forwards_warning_level_with_timestamp_and_linefeed` |
| `diagnostic_slot_forwards_info_level_with_timestamp_and_linefeed` | `TestLoggingEngine.diagnostic_slot_forwards_info_level_with_timestamp_and_linefeed` |
| `diagnostic_slot_forwards_debug_level_with_timestamp_and_linefeed` | `TestLoggingEngine.diagnostic_slot_forwards_debug_level_with_timestamp_and_linefeed` |
| `portable_events_map_to_existing_status_and_value_signals` | `TestLoggingEngine.portable_events_map_to_existing_status_and_value_signals` |

Named rows: `active run` → `active_run`, `unknown ID` → `unknown_ID`, `null factory value` → `null_factory_value`, `returned error` → `returned_error`, `std exception` → `std_exception`, `unknown exception` → `unknown_exception`.

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/logging/logging_worker_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `forwards_portable_states_samples_and_cancelled_result` | `TestLoggingWorker.forwards_portable_states_samples_and_cancelled_result` |
| `forwards_final_start_error_without_policy_mapping` | `TestLoggingWorker.forwards_final_start_error_without_policy_mapping` |
| `destruction_cancels_and_joins_a_blocked_poll` | `TestLoggingWorker.destruction_cancels_and_joins_a_blocked_poll` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/desktop_serial_factory_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `directConnectionBuildsTheDirectBackend` | `DesktopSerialFactoryTest.directConnectionBuildsTheDirectBackend` |
| `remoteConnectionBuildsTheRemoteBackend` | `DesktopSerialFactoryTest.remoteConnectionBuildsTheRemoteBackend` |
| `everyLogLevelReachesTheSink` | `DesktopSerialFactoryTest.everyLogLevelReachesTheSink` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/direct_backend_hooks_unix_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `resolvePort_prefixesAndSplitsAnAdapterEntry` | `TestDirectBackendHooksUnix.resolvePort_prefixesAndSplitsAnAdapterEntry` |
| `resolvePort_plainSerialEntryIsNotJ2534` | `TestDirectBackendHooksUnix.resolvePort_plainSerialEntryIsNotJ2534` |
| `appendJ2534Interfaces_leavesTheListUntouched` | `TestDirectBackendHooksUnix.appendJ2534Interfaces_leavesTheListUntouched` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/direct_backend_hooks_windows_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `resolvePort_keepsTheVendorNameWhole` | `TestDirectBackendHooksWindows.resolvePort_keepsTheVendorNameWhole` |
| `resolvePort_emptyEntryIsNotJ2534` | `TestDirectBackendHooksWindows.resolvePort_emptyEntryIsNotJ2534` |
| `txDone_isAlwaysTrue` | `TestDirectBackendHooksWindows.txDone_isAlwaysTrue` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/direct_backend_pty_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `ptyRead_reassemblesFragmentedFrame` | `TestDirectBackendPty.ptyRead_reassemblesFragmentedFrame` |
| `ptyRead_timesOutCleanOnSilence` | `TestDirectBackendPty.ptyRead_timesOutCleanOnSilence` |
| `ptyClearRxBuffer_discardsPendingBytes` | `TestDirectBackendPty.ptyClearRxBuffer_discardsPendingBytes` |
| `ptyAdapterVanish_readReturnsCleanly` | `TestDirectBackendPty.ptyAdapterVanish_readReturnsCleanly` |
| `ptyParityChangesWhileOpen` | `TestDirectBackendPty.ptyParityChangesWhileOpen` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/direct_backend_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `getSet_roundtrip_throughInterface` | `TestDirectBackend.getSet_roundtrip_throughInterface` |
| `closedPort_ioCalls_returnEmpty` | `TestDirectBackend.closedPort_ioCalls_returnEmpty` |
| `j2534Selection_usesInstalledDllPathAfterVendorProbe` | `TestDirectBackend.j2534Selection_usesInstalledDllPathAfterVendorProbe` |
| `j2534DriverViews_wow6432NodeVendorIsDiscoverable` | `TestDirectBackend.j2534DriverViews_wow6432NodeVendorIsDiscoverable` |
| `j2534DriverViews_laterViewOverwritesOnCollision` | `TestDirectBackend.j2534DriverViews_laterViewOverwritesOnCollision` |
| `makeDirectSerialBackend_buildsTheDirectBackend` | `TestDirectBackend.makeDirectSerialBackend_buildsTheDirectBackend` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/facade_threading_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `constructDestroy_withoutUse_noThreadNoHang` | `TestFacadeThreading.constructDestroy_withoutUse_noThreadNoHang` |
| `getSet_marshalsToBackendThread` | `TestFacadeThreading.getSet_marshalsToBackendThread` |
| `scriptedRead_returnsThroughFacade` | `TestFacadeThreading.scriptedRead_returnsThroughFacade` |
| `backendException_propagatesWithoutHangingAndCleansUp` | `TestFacadeThreading.backendException_propagatesWithoutHangingAndCleansUp` |
| `transportAdapters_isOpenContainsBackendException` | `TestFacadeThreading.transportAdapters_isOpenContainsBackendException` |
| `transportAdapters_normalEmptyReadIsSuccess` | `TestFacadeThreading.transportAdapters_normalEmptyReadIsSuccess` |
| `transportAdapters_preCancelledReadSkipsBackend` | `TestFacadeThreading.transportAdapters_preCancelledReadSkipsBackend` |
| `transportAdapters_postCallCancellationPrecedesDisconnect` | `TestFacadeThreading.transportAdapters_postCallCancellationPrecedesDisconnect` |
| `transportAdapters_backendReadExceptionMapsToInternal` | `TestFacadeThreading.transportAdapters_backendReadExceptionMapsToInternal` |
| `canTransport_truncatedFrameMapsToInternal` | `TestFacadeThreading.canTransport_truncatedFrameMapsToInternal` |
| `transportAdapters_nullOrClosedAdapterReturnsDisconnectedBeforeOperation` | `TestFacadeThreading.transportAdapters_nullOrClosedAdapterReturnsDisconnectedBeforeOperation` |
| `transportAdapters_writeSuccessAndCanFrameEncoding` | `TestFacadeThreading.transportAdapters_writeSuccessAndCanFrameEncoding` |
| `transportAdapters_disconnectDuringWriteMapsToDisconnected` | `TestFacadeThreading.transportAdapters_disconnectDuringWriteMapsToDisconnected` |
| `transportAdapters_disconnectDuringReadMapsToDisconnected` | `TestFacadeThreading.transportAdapters_disconnectDuringReadMapsToDisconnected` |
| `transportAdapters_backendWriteExceptionMapsToInternal` | `TestFacadeThreading.transportAdapters_backendWriteExceptionMapsToInternal` |
| `transportAdapters_backendNonStandardExceptionMapsToInternal` | `TestFacadeThreading.transportAdapters_backendNonStandardExceptionMapsToInternal` |
| `transportAdapters_cancellationPrecedesReadException` | `TestFacadeThreading.transportAdapters_cancellationPrecedesReadException` |
| `klineTransport_setBaudSuccessRejectionDisconnectException` | `TestFacadeThreading.klineTransport_setBaudSuccessRejectionDisconnectException` |
| `workerThreadCaller_noAffinityWarnings` | `TestFacadeThreading.workerThreadCaller_noAffinityWarnings` |
| `concurrentCallers_serializeWithoutInterleaving` | `TestFacadeThreading.concurrentCallers_serializeWithoutInterleaving` |
| `destroyAfterUse_joinsIoThread` | `TestFacadeThreading.destroyAfterUse_joinsIoThread` |
| `destroyWhileReadInFlight_waitsForBackendCall` | `TestFacadeThreading.destroyWhileReadInFlight_waitsForBackendCall` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/j2534_driver_selection_unix_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `capableEntry_matchesOnlyTheAdapterDescription` | `TestJ2534DriverSelectionUnix.capableEntry_matchesOnlyTheAdapterDescription` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/j2534_driver_selection_windows_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `capableEntry_acceptsEveryNonEmptyEntry` | `TestJ2534DriverSelectionWindows.capableEntry_acceptsEveryNonEmptyEntry` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/remote_backend_smoke_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `constructAndDestroy_localPeer_noBlockNoCrash` | `TestRemoteBackendSmoke.constructAndDestroy_localPeer_noBlockNoCrash` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/serial_idle_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `resetsTheConnectionThenRestoresTheIdleLineSettingsInOrder` | `SerialIdleTest.resetsTheConnectionThenRestoresTheIdleLineSettingsInOrder` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/testing/fake_backed_serial_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `theBackendIsLiveAsSoonAsTheFixtureIsConstructed` | `TestFakeBackedSerial.theBackendIsLiveAsSoonAsTheFixtureIsConstructed` |
| `arrangeRunsBeforeTheFixtureTouchesTheBackend` | `TestFakeBackedSerial.arrangeRunsBeforeTheFixtureTouchesTheBackend` |
| `releaseTransfersTheFacadeAndLeavesTheFakeReachable` | `TestFakeBackedSerial.releaseTransfersTheFacadeAndLeavesTheFakeReachable` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/serial/testing/fake_backend_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `defaultActionsPreserveConfigurationThroughFacade` | `FakeBackendTest.defaultActionsPreserveConfigurationThroughFacade` |
| `expectationsScriptFacadeIoInOrder` | `FakeBackendTest.expectationsScriptFacadeIoInOrder` |
| `expectationFailuresProduceNonzeroExit` | `FakeBackendTest.expectationFailuresProduceNonzeroExit` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/service_functions/serial_facade_configurator_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `isoConfigurationClearsAStaleKlineHeaderAndUsesTheRequiredOrder` | `SerialFacadeConfiguratorTest.isoConfigurationClearsAStaleKlineHeaderAndUsesTheRequiredOrder` |
| `klineConfigurationPreservesLegacyOpenBaudHeaderOrder` | `SerialFacadeConfiguratorTest.klineConfigurationPreservesLegacyOpenBaudHeaderOrder` |
| `nullFacadeIsDisconnected` | `SerialFacadeConfiguratorTest.nullFacadeIsDisconnected` |
| `anEmptyOpenResultIsDisconnectedEvenWithAStaleOpenFlag` | `SerialFacadeConfiguratorTest.anEmptyOpenResultIsDisconnectedEvenWithAStaleOpenFlag` |
| `aPortThatIsNotOpenAfterOpenIsDisconnected` | `SerialFacadeConfiguratorTest.aPortThatIsNotOpenAfterOpenIsDisconnected` |
| `eachBooleanSetterFailureIsInvalidConfig` | `eachBooleanSetterFailureIsInvalidConfigParameters.eachBooleanSetterFailureIsInvalidConfig` |
| `aKlineHeaderSetterFailureIsInvalidConfig` | `SerialFacadeConfiguratorTest.aKlineHeaderSetterFailureIsInvalidConfig` |
| `aSetterExceptionBecomesInternalStatus` | `SerialFacadeConfiguratorTest.aSetterExceptionBecomesInternalStatus` |
| `anOpenExceptionBecomesInternalStatus` | `SerialFacadeConfiguratorTest.anOpenExceptionBecomesInternalStatus` |
| `aRejectedBaudChangeIsInternal` | `SerialFacadeConfiguratorTest.aRejectedBaudChangeIsInternal` |
| `aPortDropDuringRejectedBaudChangeIsDisconnected` | `SerialFacadeConfiguratorTest.aPortDropDuringRejectedBaudChangeIsDisconnected` |
| `aStandardFacadeExceptionBecomesInternalStatus` | `SerialFacadeConfiguratorTest.aStandardFacadeExceptionBecomesInternalStatus` |
| `aNonStandardFacadeExceptionBecomesInternalStatus` | `SerialFacadeConfiguratorTest.aNonStandardFacadeExceptionBecomesInternalStatus` |

Named rows: `set_is_iso14230_connection` → `set_is_iso14230_connection`, `set_is_can_connection` → `set_is_can_connection`, `set_is_iso15765_connection` → `set_is_iso15765_connection`, `set_is_29_bit_id` → `set_is_29_bit_id`, `set_add_iso14230_header` → `set_add_iso14230_header`, `set_can_speed` → `set_can_speed`, `set_iso15765_source_address` → `set_iso15765_source_address`, `set_iso15765_destination_address` → `set_iso15765_destination_address`, `set_can_source_address` → `set_can_source_address`, `set_can_destination_address` → `set_can_destination_address`.

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/service_functions/service_function_worker_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `completesWithoutEverRequestingAGate` | `ServiceFunctionWorkerTest.completesWithoutEverRequestingAGate` |
| `appliesTheSessionsTransportConfigurationBeforeRunning` | `ServiceFunctionWorkerTest.appliesTheSessionsTransportConfigurationBeforeRunning` |
| `neverTouchesTheSerialFacadeWhenSetupFails` | `ServiceFunctionWorkerTest.neverTouchesTheSerialFacadeWhenSetupFails` |
| `blocksOnAGateUntilItIsAnswered` | `ServiceFunctionWorkerTest.blocksOnAGateUntilItIsAnswered` |
| `aDeclinedGateReachesTheSessionAsDecline` | `ServiceFunctionWorkerTest.aDeclinedGateReachesTheSessionAsDecline` |
| `requestStopUnblocksAnOutstandingGate` | `ServiceFunctionWorkerTest.requestStopUnblocksAnOutstandingGate` |
| `aStaleAnswerCannotSatisfyALaterGate` | `ServiceFunctionWorkerTest.aStaleAnswerCannotSatisfyALaterGate` |
| `emitsFinishedExactlyOnceOnFailure` | `ServiceFunctionWorkerTest.emitsFinishedExactlyOnceOnFailure` |
| `destructorDoesNotDestroyOwnedStateWhileResumeIsActive` | `ServiceFunctionWorkerTest.destructorDoesNotDestroyOwnedStateWhileResumeIsActive` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure` | `TestDesktopCanFlashTransport.configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure` |
| `configureFailsAtEachRemainingSetterInTurn` | `configureFailsAtEachRemainingSetterInTurnParameters.configureFailsAtEachRemainingSetterInTurn` |
| `openFailureReturnsDisconnectedWithoutAnyWrite` | `TestDesktopCanFlashTransport.openFailureReturnsDisconnectedWithoutAnyWrite` |
| `configureSucceedsWhenEverySetterSucceeds` | `TestDesktopCanFlashTransport.configureSucceedsWhenEverySetterSucceeds` |
| `configureClearsStickyIso14230HeaderState` | `TestDesktopCanFlashTransport.configureClearsStickyIso14230HeaderState` |
| `openSucceedsWhenBackendReturnsANonEmptyPortName` | `TestDesktopCanFlashTransport.openSucceedsWhenBackendReturnsANonEmptyPortName` |
| `resetConnectionSucceedsAndReachesTheAdapter` | `TestDesktopCanFlashTransport.resetConnectionSucceedsAndReachesTheAdapter` |
| `restartIso15765ResetsConfiguresAndReopensInOrder` | `TestDesktopCanFlashTransport.restartIso15765ResetsConfiguresAndReopensInOrder` |
| `restartIso15765CancellationBeforeResetTouchesNoBackendOperation` | `TestDesktopCanFlashTransport.restartIso15765CancellationBeforeResetTouchesNoBackendOperation` |
| `restartIso15765ResetFailureStopsBeforeConfigurationOrOpen` | `TestDesktopCanFlashTransport.restartIso15765ResetFailureStopsBeforeConfigurationOrOpen` |
| `restartIso15765ConfigureFailureStopsBeforeOpen` | `TestDesktopCanFlashTransport.restartIso15765ConfigureFailureStopsBeforeOpen` |
| `restartIso15765OpenFailurePropagatesAfterExactConfiguration` | `TestDesktopCanFlashTransport.restartIso15765OpenFailurePropagatesAfterExactConfiguration` |
| `resetConnectionReturnsDisconnectedAfterClose` | `TestDesktopCanFlashTransport.resetConnectionReturnsDisconnectedAfterClose` |
| `resetConnectionMapsStandardDriverExceptionsToInternal` | `TestDesktopCanFlashTransport.resetConnectionMapsStandardDriverExceptionsToInternal` |
| `resetConnectionMapsNonStandardDriverExceptionsToInternal` | `TestDesktopCanFlashTransport.resetConnectionMapsNonStandardDriverExceptionsToInternal` |
| `writeSucceedsWhenPortStaysOpenThroughout` | `TestDesktopCanFlashTransport.writeSucceedsWhenPortStaysOpenThroughout` |
| `writeReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingWrite` | `TestDesktopCanFlashTransport.writeReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingWrite` |
| `writeFailsWithDisconnectedWhenPortClosesDuringWrite` | `TestDesktopCanFlashTransport.writeFailsWithDisconnectedWhenPortClosesDuringWrite` |
| `writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite` | `TestDesktopCanFlashTransport.writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite` |
| `readReturnsScriptedBytesOnSuccess` | `TestDesktopCanFlashTransport.readReturnsScriptedBytesOnSuccess` |
| `readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead` | `TestDesktopCanFlashTransport.readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead` |
| `readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead` | `TestDesktopCanFlashTransport.readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead` |
| `readReturnsDisconnectedWhenPortClosesDuringRead` | `TestDesktopCanFlashTransport.readReturnsDisconnectedWhenPortClosesDuringRead` |
| `everyMethodFailsWithDisconnectedAfterClose` | `TestDesktopCanFlashTransport.everyMethodFailsWithDisconnectedAfterClose` |
| `writeIsSkippedWithCancelledAfterRequestUnblock` | `TestDesktopCanFlashTransport.writeIsSkippedWithCancelledAfterRequestUnblock` |
| `readReturnsEmptyOptionalWhenBackendReturnsNoBytes` | `TestDesktopCanFlashTransport.readReturnsEmptyOptionalWhenBackendReturnsNoBytes` |
| `writeFailsWithInternalWhenDriverThrowsStandardException` | `TestDesktopCanFlashTransport.writeFailsWithInternalWhenDriverThrowsStandardException` |
| `writeFailsWithInternalWhenDriverThrowsNonStandardException` | `TestDesktopCanFlashTransport.writeFailsWithInternalWhenDriverThrowsNonStandardException` |
| `readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled` | `TestDesktopCanFlashTransport.readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled` |
| `readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled` | `TestDesktopCanFlashTransport.readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled` |
| `readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead` | `TestDesktopCanFlashTransport.readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead` |
| `readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow` | `TestDesktopCanFlashTransport.readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow` |
| `readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow` | `TestDesktopCanFlashTransport.readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow` |
| `closeIsIdempotentAndDestroysTheOwnedSerialPortActions` | `TestDesktopCanFlashTransport.closeIsIdempotentAndDestroysTheOwnedSerialPortActions` |
| `closeOnANonOwningSerialPortActionsDoesNotDestroyIt` | `TestDesktopCanFlashTransport.closeOnANonOwningSerialPortActionsDoesNotDestroyIt` |
| `requestUnblockCausesAPendingReadToReturnPromptly` | `TestDesktopCanFlashTransport.requestUnblockCausesAPendingReadToReturnPromptly` |
| `fakeBackendReportsScriptedPortListAndBattery` | `TestDesktopCanFlashTransport.fakeBackendReportsScriptedPortListAndBattery` |

Named rows: `set_is_iso15765_connection` → `set_is_iso15765_connection`, `set_is_can_connection` → `set_is_can_connection`, `set_is_iso14230_connection` → `set_is_iso14230_connection`, `set_is_29_bit_id` → `set_is_29_bit_id`, `set_can_source_address` → `set_can_source_address`, `set_can_destination_address` → `set_can_destination_address`, `set_iso15765_source_address` → `set_iso15765_source_address`, `set_iso15765_destination_address` → `set_iso15765_destination_address`, `set_add_iso14230_header` → `set_add_iso14230_header`.

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/transport/desktop_kline_flash_transport_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `configureSetsAndResetsParityOnReusedFacade` | `TestDesktopKlineFlashTransport.configureSetsAndResetsParityOnReusedFacade` |
| `configureReportsParitySetterFailure` | `TestDesktopKlineFlashTransport.configureReportsParitySetterFailure` |
| `rawCallsUseRawSerialMethods` | `TestDesktopKlineFlashTransport.rawCallsUseRawSerialMethods` |
| `postKernelUploadDelayCapabilityMirrorsOpenPort2OnUnix` | `TestDesktopKlineFlashTransport.postKernelUploadDelayCapabilityMirrorsOpenPort2OnUnix` |
| `programmingVoltageSupplyMirrorsOpenPort2OnEveryPlatform` | `TestDesktopKlineFlashTransport.programmingVoltageSupplyMirrorsOpenPort2OnEveryPlatform` |
| `resetConnectionReachesTheAdapter` | `TestDesktopKlineFlashTransport.resetConnectionReachesTheAdapter` |
| `resetConnectionAfterCloseIsDisconnectedAndTouchesNoBackend` | `TestDesktopKlineFlashTransport.resetConnectionAfterCloseIsDisconnectedAndTouchesNoBackend` |
| `lecControlOperationsForwardToSerialBackend` | `TestDesktopKlineFlashTransport.lecControlOperationsForwardToSerialBackend` |
| `configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure` | `TestDesktopKlineFlashTransport.configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure` |
| `configureFailsAtEachRemainingSetterInTurn` | `configureFailsAtEachRemainingSetterInTurnParameters.configureFailsAtEachRemainingSetterInTurn` |
| `openFailureReturnsDisconnectedWithoutAnyWrite` | `TestDesktopKlineFlashTransport.openFailureReturnsDisconnectedWithoutAnyWrite` |
| `configureSucceedsWhenEverySetterSucceeds` | `TestDesktopKlineFlashTransport.configureSucceedsWhenEverySetterSucceeds` |
| `openSucceedsWhenBackendReturnsANonEmptyPortName` | `TestDesktopKlineFlashTransport.openSucceedsWhenBackendReturnsANonEmptyPortName` |
| `setBaudSucceedsWhenPortOpenAndDriverReturnsSuccess` | `TestDesktopKlineFlashTransport.setBaudSucceedsWhenPortOpenAndDriverReturnsSuccess` |
| `setBaudFailsWithInternalWhenPortStaysOpenButDriverRejectsChange` | `TestDesktopKlineFlashTransport.setBaudFailsWithInternalWhenPortStaysOpenButDriverRejectsChange` |
| `setBaudFailsWithDisconnectedWhenPortAlreadyClosed` | `TestDesktopKlineFlashTransport.setBaudFailsWithDisconnectedWhenPortAlreadyClosed` |
| `setBaudFailsWithDisconnectedWhenPortClosesDuringBaudChange` | `TestDesktopKlineFlashTransport.setBaudFailsWithDisconnectedWhenPortClosesDuringBaudChange` |
| `writeSucceedsAndReturnsRequestedByteCount` | `TestDesktopKlineFlashTransport.writeSucceedsAndReturnsRequestedByteCount` |
| `writeFailsWithDisconnectedWhenPortClosesDuringWrite` | `TestDesktopKlineFlashTransport.writeFailsWithDisconnectedWhenPortClosesDuringWrite` |
| `readReturnsScriptedBytesOnSuccess` | `TestDesktopKlineFlashTransport.readReturnsScriptedBytesOnSuccess` |
| `readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead` | `TestDesktopKlineFlashTransport.readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead` |
| `readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead` | `TestDesktopKlineFlashTransport.readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead` |
| `readReturnsDisconnectedWhenPortClosesDuringRead` | `TestDesktopKlineFlashTransport.readReturnsDisconnectedWhenPortClosesDuringRead` |
| `everyMethodFailsWithDisconnectedAfterClose` | `TestDesktopKlineFlashTransport.everyMethodFailsWithDisconnectedAfterClose` |
| `setAddIso14230HeaderForwardsToSerialAndSucceeds` | `TestDesktopKlineFlashTransport.setAddIso14230HeaderForwardsToSerialAndSucceeds` |
| `writeIsSkippedWithCancelledAfterRequestUnblock` | `TestDesktopKlineFlashTransport.writeIsSkippedWithCancelledAfterRequestUnblock` |
| `writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite` | `TestDesktopKlineFlashTransport.writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite` |
| `readReturnsEmptyOptionalWhenBackendReturnsNoBytes` | `TestDesktopKlineFlashTransport.readReturnsEmptyOptionalWhenBackendReturnsNoBytes` |
| `setBaudFailsWithInternalWhenDriverThrowsStandardException` | `TestDesktopKlineFlashTransport.setBaudFailsWithInternalWhenDriverThrowsStandardException` |
| `setBaudFailsWithInternalWhenDriverThrowsNonStandardException` | `TestDesktopKlineFlashTransport.setBaudFailsWithInternalWhenDriverThrowsNonStandardException` |
| `writeFailsWithInternalWhenDriverThrowsStandardException` | `TestDesktopKlineFlashTransport.writeFailsWithInternalWhenDriverThrowsStandardException` |
| `writeFailsWithInternalWhenDriverThrowsNonStandardException` | `TestDesktopKlineFlashTransport.writeFailsWithInternalWhenDriverThrowsNonStandardException` |
| `readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled` | `TestDesktopKlineFlashTransport.readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled` |
| `readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled` | `TestDesktopKlineFlashTransport.readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled` |
| `isOpenReflectsThePortsRealOpenState` | `TestDesktopKlineFlashTransport.isOpenReflectsThePortsRealOpenState` |
| `isOpenReturnsFalseWhenTheUnderlyingCheckThrows` | `TestDesktopKlineFlashTransport.isOpenReturnsFalseWhenTheUnderlyingCheckThrows` |
| `isOpenReturnsFalseAfterClose` | `TestDesktopKlineFlashTransport.isOpenReturnsFalseAfterClose` |
| `readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead` | `TestDesktopKlineFlashTransport.readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead` |
| `readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow` | `TestDesktopKlineFlashTransport.readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow` |
| `readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow` | `TestDesktopKlineFlashTransport.readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow` |
| `closeIsIdempotentAndDestroysTheOwnedSerialPortActions` | `TestDesktopKlineFlashTransport.closeIsIdempotentAndDestroysTheOwnedSerialPortActions` |
| `closeOnANonOwningSerialPortActionsDoesNotDestroyIt` | `TestDesktopKlineFlashTransport.closeOnANonOwningSerialPortActionsDoesNotDestroyIt` |
| `requestUnblockCausesAPendingReadToReturnPromptly` | `TestDesktopKlineFlashTransport.requestUnblockCausesAPendingReadToReturnPromptly` |

Named rows: `set_is_iso14230_connection` → `set_is_iso14230_connection`, `set_is_can_connection` → `set_is_can_connection`, `set_is_29_bit_id` → `set_is_29_bit_id`, `set_serial_port_baudrate` → `set_serial_port_baudrate`.

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/transport/desktop_logging_protocol_registration_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `registration_performs_no_io` | `DesktopLoggingProtocolRegistrationTest.registration_performs_no_io` |
| `cdbg_setup_failure_stops_at_failed_step` | `DesktopLoggingProtocolRegistrationTest.cdbg_setup_failure_stops_at_failed_step` |
| `cdbg_open_failure` | `DesktopLoggingProtocolRegistrationTest.cdbg_open_failure` |
| `cdbg_success_preserves_start_sequence` | `DesktopLoggingProtocolRegistrationTest.cdbg_success_preserves_start_sequence` |
| `ssm_target_and_adapter_are_per_run` | `DesktopLoggingProtocolRegistrationTest.ssm_target_and_adapter_are_per_run` |
| `ssm_snapshot_offsets_reach_samples` | `DesktopLoggingProtocolRegistrationTest.ssm_snapshot_offsets_reach_samples` |
| `mut_dma_preserves_initialization_and_channels` | `DesktopLoggingProtocolRegistrationTest.mut_dma_preserves_initialization_and_channels` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/transport/desktop_mixed_can_flash_transport_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `initialResetReachesBackendAndReturnsFailure` | `TestDesktopMixedCanFlashTransport.initialResetReachesBackendAndReturnsFailure` |
| `initialResetAfterCloseReturnsDisconnected` | `TestDesktopMixedCanFlashTransport.initialResetAfterCloseReturnsDisconnected` |
| `configuresIsoThenTransitionsRawAndBack` | `TestDesktopMixedCanFlashTransport.configuresIsoThenTransitionsRawAndBack` |
| `everyModeConfigurationClearsStickyIso14230HeaderState` | `TestDesktopMixedCanFlashTransport.everyModeConfigurationClearsStickyIso14230HeaderState` |
| `preservesExtendedIsoIdDuringInitialConfigurationAndReturnTransition` | `TestDesktopMixedCanFlashTransport.preservesExtendedIsoIdDuringInitialConfigurationAndReturnTransition` |
| `rawFrameAddsAndParsesBigEndianId` | `TestDesktopMixedCanFlashTransport.rawFrameAddsAndParsesBigEndianId` |
| `configureFailsAtEverySetter` | `TestDesktopMixedCanFlashTransport.configureFailsAtEverySetter` |
| `configureRejectsReconfigureWhileAlreadyConfigured` | `TestDesktopMixedCanFlashTransport.configureRejectsReconfigureWhileAlreadyConfigured` |
| `poisonedTransitionMakesConfigureAndOpenSurfaceTheStaleErrorEvenAfterClose` | `TestDesktopMixedCanFlashTransport.poisonedTransitionMakesConfigureAndOpenSurfaceTheStaleErrorEvenAfterClose` |
| `rawTransitionFailsAtEverySetterAndMakesIoTerminal` | `TestDesktopMixedCanFlashTransport.rawTransitionFailsAtEverySetterAndMakesIoTerminal` |
| `failedReopenMakesFollowingIoTerminal` | `TestDesktopMixedCanFlashTransport.failedReopenMakesFollowingIoTerminal` |
| `rawReadRejectsShortFrameAndWrongReceiveId` | `TestDesktopMixedCanFlashTransport.rawReadRejectsShortFrameAndWrongReceiveId` |
| `clearReceiveBufferRejectsBackendFailure` | `TestDesktopMixedCanFlashTransport.clearReceiveBufferRejectsBackendFailure` |
| `detectsDisconnectionBeforeAndDuringIo` | `TestDesktopMixedCanFlashTransport.detectsDisconnectionBeforeAndDuringIo` |
| `catchesStandardAndNonstandardBackendExceptions` | `TestDesktopMixedCanFlashTransport.catchesStandardAndNonstandardBackendExceptions` |
| `cancellationAndUnblockSuppressSubsequentIo` | `TestDesktopMixedCanFlashTransport.cancellationAndUnblockSuppressSubsequentIo` |
| `nonOwningCloseDoesNotDestroyCallerSerial` | `TestDesktopMixedCanFlashTransport.nonOwningCloseDoesNotDestroyCallerSerial` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/platform/desktop/common/transport/desktop_transport_factory_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `listsEveryDetectedPort` | `TestDesktopTransportFactory.listsEveryDetectedPort` |
| `refusesToOpenWhenNoDeviceIsDetected` | `TestDesktopTransportFactory.refusesToOpenWhenNoDeviceIsDetected` |
| `refusesToOpenWhenTheNamedDeviceIsAbsent` | `TestDesktopTransportFactory.refusesToOpenWhenTheNamedDeviceIsAbsent` |
| `selectsTheFirstJ2534DeviceWhenNoNameIsGiven` | `TestDesktopTransportFactory.selectsTheFirstJ2534DeviceWhenNoNameIsGiven` |
| `skipsNonJ2534PortsWhenNoNameIsGiven` | `TestDesktopTransportFactory.skipsNonJ2534PortsWhenNoNameIsGiven` |
| `refusesToOpenWhenNoDetectedPortIsAJ2534Adapter` | `TestDesktopTransportFactory.refusesToOpenWhenNoDetectedPortIsAJ2534Adapter` |
| `refusesToOpenWhenTheNamedDeviceIsNotAJ2534Adapter` | `TestDesktopTransportFactory.refusesToOpenWhenTheNamedDeviceIsNotAJ2534Adapter` |
| `reportsDisconnectedWhenTheOpenFails` | `TestDesktopTransportFactory.reportsDisconnectedWhenTheOpenFails` |
| `refusesAConfigWithoutABackendFactory` | `TestDesktopTransportFactory.refusesAConfigWithoutABackendFactory` |

Conditional skip sites: 0 → 0.

Application: Core.

## src/ui/desktop/calibration_maps_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `layoutsAndRefresh` | `layoutsAndRefreshParameters.layoutsAndRefresh` |
| `staticAxisLabels` | `staticAxisLabelsParameters.staticAxisLabels` |
| `absentAxisUsesSequentialFallback` | `CalibrationMapsTest.absentAxisUsesSequentialFallback` |
| `selectableReflectsBlobBytesWithoutEmittingEditSignal` | `CalibrationMapsTest.selectableReflectsBlobBytesWithoutEmittingEditSignal` |
| `retainedMultiSelectableGeometryKeepsLegacyNumericCell` | `CalibrationMapsTest.retainedMultiSelectableGeometryKeepsLegacyNumericCell` |
| `retainedSwitchRefreshKeepsUncheckedControlWithoutEmittingEdits` | `CalibrationMapsTest.retainedSwitchRefreshKeepsUncheckedControlWithoutEmittingEdits` |
| `colorsKeepOpeningBoundsDuringRefreshAndReopenUsesCurrentValues` | `CalibrationMapsTest.colorsKeepOpeningBoundsDuringRefreshAndReopenUsesCurrentValues` |
| `constantMapHasFiniteStableColors` | `CalibrationMapsTest.constantMapHasFiniteStableColors` |
| `closedSessionRefreshIsInertAfterAnotherSessionOpens` | `CalibrationMapsTest.closedSessionRefreshIsInertAfterAnotherSessionOpens` |

Named rows: `1D` → `1D`, `X 2D` → `X_2D`, `Y 2D` → `Y_2D`, `3D` → `3D`, `static X` → `static_X`, `static Y` → `static_Y`.

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/calibration_treewidget_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `filesTreeCarriesNameFirstMapIdAndSessionKey` | `CalibrationTreeWidgetTest.filesTreeCarriesNameFirstMapIdAndSessionKey` |
| `dataTreeMatchesLegacyRules` | `CalibrationTreeWidgetTest.dataTreeMatchesLegacyRules` |
| `definitionlessRomShowsOnlyRomInfo` | `CalibrationTreeWidgetTest.definitionlessRomShowsOnlyRomInfo` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/dtc_operations_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `aFailedRunLogsOnceAndReenablesTheButtons` | `DtcOperationsTest.aFailedRunLogsOnceAndReenablesTheButtons` |
| `closeDuringARunStopsTheWorkerAndResets` | `DtcOperationsTest.closeDuringARunStopsTheWorkerAndResets` |
| `escapeDuringARunStopsTheWorkerAndResets` | `DtcOperationsTest.escapeDuringARunStopsTheWorkerAndResets` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/flash/common/flash_dialog_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `returnsAcceptedBytesAndUsesNormalizedReadTitle` | `FlashDialogTest.returnsAcceptedBytesAndUsesNormalizedReadTitle` |
| `closingMidAttemptSubmitsCancelledAndPresentsTheNotice` | `FlashDialogTest.closingMidAttemptSubmitsCancelledAndPresentsTheNotice` |
| `runsASecondAttemptAfterAPromptBetweenAttempts` | `FlashDialogTest.runsASecondAttemptAfterAPromptBetweenAttempts` |
| `programmingVoltageNoticeKeepsTheSixC3AdviceByDefault` | `FlashDialogTest.programmingVoltageNoticeKeepsTheSixC3AdviceByDefault` |
| `programmingVoltageNoticeWithoutPowerOffAdviceOnFailure` | `FlashDialogTest.programmingVoltageNoticeWithoutPowerOffAdviceOnFailure` |
| `programmingVoltageNoticeWithoutPowerOffAdviceOnSuccess` | `FlashDialogTest.programmingVoltageNoticeWithoutPowerOffAdviceOnSuccess` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/flash/operation/flash_operation_controller_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `unknownProtocolIsUnsupportedAndWarnsWithoutSerialIo` | `FlashOperationControllerTest.unknownProtocolIsUnsupportedAndWarnsWithoutSerialIo` |
| `cancelledDensoTcuChooserIsHandledWithoutSerialIo` | `FlashOperationControllerTest.cancelledDensoTcuChooserIsHandledWithoutSerialIo` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/mainwindow_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `explicitConfigRootLoadsFixtureAndProvisionsDirectories` | `MainWindowTest.explicitConfigRootLoadsFixtureAndProvisionsDirectories` |
| `directSessionStartupNeverWaitsForARemoteSource` | `MainWindowTest.directSessionStartupNeverWaitsForARemoteSource` |
| `windowLogLinesReachTheLogChannel` | `MainWindowTest.windowLogLinesReachTheLogChannel` |
| `windowEnablesFileLoggingThroughTheChannel` | `MainWindowTest.windowEnablesFileLoggingThroughTheChannel` |
| `directSessionStartupNeverRequestsTheRemoteWait` | `MainWindowTest.directSessionStartupNeverRequestsTheRemoteWait` |
| `externalLoggerMirrorsToTheRemotePeer` | `MainWindowTest.externalLoggerMirrorsToTheRemotePeer` |
| `peerStateChangesReachTheWindow` | `MainWindowTest.peerStateChangesReachTheWindow` |
| `handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling` | `handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePollingParameters.handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling` |
| `futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo` | `futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIoParameters.futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo` |
| `representativePortableRoutesReachFactoryBeforeLegacyFallback` | `representativePortableRoutesReachFactoryBeforeLegacyFallbackParameters.representativePortableRoutesReachFactoryBeforeLegacyFallback` |
| `writeWithoutASelectedCalibrationStopsVoltagePolling` | `MainWindowTest.writeWithoutASelectedCalibrationStopsVoltagePolling` |
| `otherMakesSkipDispatchButStillRunCleanup` | `MainWindowTest.otherMakesSkipDispatchButStillRunCleanup` |
| `readOfAnUnsupportedProtocolAddsNoCalibration` | `MainWindowTest.readOfAnUnsupportedProtocolAddsNoCalibration` |
| `cancellingTheChecksumWarningStopsVoltagePolling` | `MainWindowTest.cancellingTheChecksumWarningStopsVoltagePolling` |
| `definitionlessOpenPromptsOnceAndAppliesPlaceholders` | `MainWindowTest.definitionlessOpenPromptsOnceAndAppliesPlaceholders` |
| `closingAMiddleRomKeepsLaterRomsAddressable` | `MainWindowTest.closingAMiddleRomKeepsLaterRomsAddressable` |
| `windowsOfAClosedRomAreInert` | `MainWindowTest.windowsOfAClosedRomAreInert` |
| `hexEditorOutlivesItsRom` | `MainWindowTest.hexEditorOutlivesItsRom` |
| `closingARomClosesAllOfItsWindows` | `MainWindowTest.closingARomClosesAllOfItsWindows` |
| `viewStateIsKeptPerRom` | `MainWindowTest.viewStateIsKeptPerRom` |
| `writeMetadataFillsAnEmptyDefinitionFlashMethod` | `MainWindowTest.writeMetadataFillsAnEmptyDefinitionFlashMethod` |
| `writeMetadataLeavesADefinitionlessFlashMethodAlone` | `MainWindowTest.writeMetadataLeavesADefinitionlessFlashMethodAlone` |
| `checksumAndSaveUseATemporaryImage` | `MainWindowTest.checksumAndSaveUseATemporaryImage` |
| `saveAsChangesSourceAndTreeOnlyAfterSuccess` | `MainWindowTest.saveAsChangesSourceAndTreeOnlyAfterSuccess` |
| `selectableSignalEditsItsEmittingSession` | `MainWindowTest.selectableSignalEditsItsEmittingSession` |
| `failedMapDecodeDoesNotOccupyAView` | `MainWindowTest.failedMapDecodeDoesNotOccupyAView` |
| `windowPreservesInjectedLoggingFactory` | `MainWindowTest.windowPreservesInjectedLoggingFactory` |
| `loggingCapturesTargetForEachRun` | `MainWindowTest.loggingCapturesTargetForEachRun` |
| `chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation` | `chooserDialogsApplyAcceptedChoicesAndIgnoreCancellationParameters.chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation` |
| `definitionManagerRemovesSelectedRowsAndSavesSurvivingOrder` | `MainWindowTest.definitionManagerRemovesSelectedRowsAndSavesSurvivingOrder` |
| `numericWindowGeometryRestoresAndPersistsAcrossWindowStates` | `MainWindowTest.numericWindowGeometryRestoresAndPersistsAcrossWindowStates` |
| `acceptedVehicleChoiceSelectsTheRowAndSavesIt` | `MainWindowTest.acceptedVehicleChoiceSelectsTheRowAndSavesIt` |
| `cancelledVehicleChoiceChangesNothing` | `MainWindowTest.cancelledVehicleChoiceChangesNothing` |
| `acceptedProtocolChoiceSelectsTheLastMatchingRow` | `MainWindowTest.acceptedProtocolChoiceSelectsTheLastMatchingRow` |
| `romFlashMethodSelectsTheLastMatchingRow` | `MainWindowTest.romFlashMethodSelectsTheLastMatchingRow` |
| `unmatchedRomFlashMethodChangesNothing` | `MainWindowTest.unmatchedRomFlashMethodChangesNothing` |
| `unresolvedProtocolRowLeavesReadAndWriteUnavailable` | `MainWindowTest.unresolvedProtocolRowLeavesReadAndWriteUnavailable` |
| `loggingUsesTheSessionLogProtocol` | `MainWindowTest.loggingUsesTheSessionLogProtocol` |
| `selectedSerialPortIsEmptyWithoutPorts` | `MainWindowTest.selectedSerialPortIsEmptyWithoutPorts` |
| `dtcWindowWithoutAPortWarnsInsteadOfCrashing` | `MainWindowTest.dtcWindowWithoutAPortWarnsInsteadOfCrashing` |
| `repeatedSaveFailuresLogOnceUntilASuccess` | `MainWindowTest.repeatedSaveFailuresLogOnceUntilASuccess` |
| `biuWindowRemembersTheOpenedPort` | `MainWindowTest.biuWindowRemembersTheOpenedPort` |
| `disconnectReturnsTheAdapterToIdle` | `MainWindowTest.disconnectReturnsTheAdapterToIdle` |
| `connectOnAnotherMakeDisconnectsWithoutIdentifying` | `MainWindowTest.connectOnAnotherMakeDisconnectsWithoutIdentifying` |
| `subaruKlineConnectIdentifiesOffTheUiThread` | `MainWindowTest.subaruKlineConnectIdentifiesOffTheUiThread` |
| `subaruConnectThatNeverAnswersDisconnectsAndRestoresControls` | `MainWindowTest.subaruConnectThatNeverAnswersDisconnectsAndRestoresControls` |
| `disconnectDuringIdentificationCancelsAndDropsTheResult` | `MainWindowTest.disconnectDuringIdentificationCancelsAndDropsTheResult` |
| `loggingSelectionFailureSemanticsAndSupportPreservation` | `MainWindowTest.loggingSelectionFailureSemanticsAndSupportPreservation` |
| `loggingDefinitionFailureIsNonfatal` | `MainWindowTest.loggingDefinitionFailureIsNonfatal` |
| `unresolvedDisplaySlotsAreSkippedAndUpdateTheirOriginalLabels` | `MainWindowTest.unresolvedDisplaySlotsAreSkippedAndUpdateTheirOriginalLabels` |
| `chooserDuplicateLabelIdentity` | `chooserDuplicateLabelIdentityParameters.chooserDuplicateLabelIdentity` |
| `csvSharedIdProtocolIdentity` | `MainWindowTest.csvSharedIdProtocolIdentity` |
| `loggingStartWaitsForIdentification` | `loggingStartWaitsForIdentificationParameters.loggingStartWaitsForIdentification` |
| `batterySamplingDoesNotUseTheFacadeDuringIdentification` | `MainWindowTest.batterySamplingDoesNotUseTheFacadeDuringIdentification` |
| `windowDestructionJoinsIdentificationWithoutContinuingLogging` | `MainWindowTest.windowDestructionJoinsIdentificationWithoutContinuingLogging` |
| `connectStopsAnActiveLoggingWorkerBeforeIdentification` | `MainWindowTest.connectStopsAnActiveLoggingWorkerBeforeIdentification` |
| `connectionEntryPointsStopIdentification` | `connectionEntryPointsStopIdentificationParameters.connectionEntryPointsStopIdentification` |
| `nestedConnectDuringCapabilityNoticeKeepsEachContinuation` | `MainWindowTest.nestedConnectDuringCapabilityNoticeKeepsEachContinuation` |

Named rows: `chooser-cancelled` → `chooser_cancelled`, `relearn-declined` → `relearn_declined`, `future-can` → `future_can`, `extra-densocan` → `extra_densocan`, `petrol` → `petrol`, `densocan` → `densocan`, `denso_sh705x_kline` → `denso_sh705x_kline`, `vehicle-accept` → `vehicle_accept`, `vehicle-cancel` → `vehicle_cancel`, `protocol-accept` → `protocol_accept`, `protocol-cancel` → `protocol_cancel`, `ECU` → `ECU`, `TCU` → `TCU`, `gauge` → `gauge`, `digital` → `digital`, `switch` → `switch`.

Dynamic rows: `log_transport_changed`, `check_serial_ports`, `open_serial_port`, `show_dtc_window`, `show_subaru_biu_window`, `show_terminal_window` retain their names.

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/protocol_select_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `listsEachVehicleBackedProtocolOnce` | `ProtocolSelectTest.listsEachVehicleBackedProtocolOnce` |
| `choosingRecordsTheProtocolName` | `ProtocolSelectTest.choosingRecordsTheProtocolName` |
| `rejectingLeavesNoChoice` | `ProtocolSelectTest.rejectingLeavesNoChoice` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/service_functions/denso_tcu_read_preflight_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `chooserReturnsTheActionNamedByEachLegacyButton` | `chooserReturnsTheActionNamedByEachLegacyButtonParameters.chooserReturnsTheActionNamedByEachLegacyButton` |
| `dismissingChooserReturnsCancelled` | `DensoTcuReadPreflightTest.dismissingChooserReturnsCancelled` |
| `dumpAndCancelledReturnWithoutIgnitionOrSerialCalls` | `dumpAndCancelledReturnWithoutIgnitionOrSerialCallsParameters.dumpAndCancelledReturnWithoutIgnitionOrSerialCalls` |
| `decliningIgnitionSkipsEveryServiceDialogAndSerialCall` | `decliningIgnitionSkipsEveryServiceDialogAndSerialCallParameters.decliningIgnitionSkipsEveryServiceDialogAndSerialCall` |
| `acceptingIgnitionOpensTheMatchingRealServiceDialog` | `acceptingIgnitionOpensTheMatchingRealServiceDialogParameters.acceptingIgnitionOpensTheMatchingRealServiceDialog` |

Named rows: `dump` → `dump`, `relearn` → `relearn`, `read` → `read`, `set` → `set`, `dump` → `dump`, `cancelled` → `cancelled`, `relearn` → `relearn`, `read` → `read`, `set` → `set`, `relearn` → `relearn`, `read` → `read`, `set` → `set`.

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/service_functions/service_function_dialog_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `setParametersSpinBoxesCarryTheLegacyPromptBounds` | `ServiceFunctionDialogTest.setParametersSpinBoxesCarryTheLegacyPromptBounds` |
| `everyFormFieldLandsInItsOwnStructMember` | `ServiceFunctionDialogTest.everyFormFieldLandsInItsOwnStructMember` |
| `setParametersFormIsOneDialogNotNineModals` | `ServiceFunctionDialogTest.setParametersFormIsOneDialogNotNineModals` |
| `readParametersRendersAllNineLegacyQualifiedLabelsAndValues` | `ServiceFunctionDialogTest.readParametersRendersAllNineLegacyQualifiedLabelsAndValues` |
| `readParametersHasNoSpinBoxes` | `ServiceFunctionDialogTest.readParametersHasNoSpinBoxes` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/settings_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `providerCheckboxesPersistBothDirections` | `providerCheckboxesPersistBothDirectionsParameters.providerCheckboxesPersistBothDirections` |
| `removingDefinitionsPreservesOrderAndPersistsEmptyList` | `removingDefinitionsPreservesOrderAndPersistsEmptyListParameters.removingDefinitionsPreservesOrderAndPersistsEmptyList` |
| `closingSettingsSavesThroughTheSession` | `SettingsTest.closingSettingsSavesThroughTheSession` |
| `editsReachTheSessionLive` | `SettingsTest.editsReachTheSessionLive` |
| `destructionRetriesPersistenceAfterClose` | `SettingsTest.destructionRetriesPersistenceAfterClose` |
| `failedSaveKeepsEditsAndWarnsTheOperator` | `SettingsTest.failedSaveKeepsEditsAndWarnsTheOperator` |

Named rows: `enabled-romraider` → `enabled_romraider`, `disabled-ecuflash` → `disabled_ecuflash`, `surviving-order` → `surviving_order`, `empty-list` → `empty_list`.

Conditional skip sites: 0 → 0.

Application: Widgets.

## src/ui/desktop/vehicle_select_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `choosingRecordsTheRowWithoutTouchingTheSession` | `VehicleSelectTest.choosingRecordsTheRowWithoutTouchingTheSession` |
| `rejectingLeavesNoChoice` | `VehicleSelectTest.rejectingLeavesNoChoice` |

Conditional skip sites: 0 → 0.

Application: Widgets.

## tests/force_asserts/tst_force_asserts.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `outOfBoundsAtAborts` | `TestForceAsserts.outOfBoundsAtAborts` |

Conditional skip sites: 0 → 0.

Application: none.

## tests/serial_pty_e2e_test.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `workerThread_writeRead_overPty_deliversFramedMessage` | `TestPtyE2e.workerThread_writeRead_overPty_deliversFramedMessage` |

Conditional skip sites: 0 → 0.

Application: Core.

## tests/tst_mut_dma_integration.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `connectsOverMockPty_facadeReportsOpen` | `MutDmaIntegrationTest.connectsOverMockPty_facadeReportsOpen` |
| `setBaud_throughAdapter_trueWhenConnected_falseWhenClosed` | `MutDmaIntegrationTest.setBaud_throughAdapter_trueWhenConnected_falseWhenClosed` |
| `write_throughAdapter_putsExactFrameOnWire` | `MutDmaIntegrationTest.write_throughAdapter_putsExactFrameOnWire` |
| `read_throughAdapter_returnsEcuReplyBytes` | `MutDmaIntegrationTest.read_throughAdapter_returnsEcuReplyBytes` |
| `driverPollOnce_throughAdapter_decodesStreamFrameFromWire` | `MutDmaIntegrationTest.driverPollOnce_throughAdapter_decodesStreamFrameFromWire` |

Conditional skip sites: 0 → 0.

Application: Core.

## tests/tst_serial_port_crash.cpp

| Baseline case | GoogleTest replacement |
| --- | --- |
| `isSerialPortOpen_withNullSerial_doesNotCrash` | `SerialPortCrashTest.isSerialPortOpen_withNullSerial_doesNotCrash` |
| `readSerialData_withNullSerial_doesNotCrash` | `SerialPortCrashTest.readSerialData_withNullSerial_doesNotCrash` |
| `readSerialData_withNullSerial_doesNotBusySpin` | `SerialPortCrashTest.readSerialData_withNullSerial_doesNotBusySpin` |
| `passThruReadMsgs_withNullSerial_doesNotCrash` | `SerialPortCrashTest.passThruReadMsgs_withNullSerial_doesNotCrash` |
| `readVbatt_throughNullJ2534Serial_doesNotCrash` | `SerialPortCrashTest.readVbatt_throughNullJ2534Serial_doesNotCrash` |
| `reentrantReadDuringTeardown_viaEventLoop_doesNotCrash` | `SerialPortCrashTest.reentrantReadDuringTeardown_viaEventLoop_doesNotCrash` |
| `j2534Handshake_overMockPty_readVersionSucceeds` | `SerialPortCrashTest.j2534Handshake_overMockPty_readVersionSucceeds` |
| `spadInitJ2534Connection_overMockPty_succeeds` | `SerialPortCrashTest.spadInitJ2534Connection_overMockPty_succeeds` |
| `loggingFlow_connectReadTeardownReentrancy_overMockPty_doesNotCrash` | `SerialPortCrashTest.loggingFlow_connectReadTeardownReentrancy_overMockPty_doesNotCrash` |
| `resetQueuedDuringRead_runsAfterReadCompletes` | `SerialPortCrashTest.resetQueuedDuringRead_runsAfterReadCompletes` |
| `blockingRead_doesNotDispatchQueuedEvents` | `SerialPortCrashTest.blockingRead_doesNotDispatchQueuedEvents` |

Conditional skip sites: 0 → 0.

Application: Core.

## Existing GoogleTest signal consumers

`qt_port_adapters_test.cpp` and `definition_authoring_dialog_test.cpp` retain their GoogleTest cases and use typed signal recorders.


## Native Windows probes

- `j2534_bridge_protocol_test`: both pipe/frame cases retain their names.
- `pe_bitness_test`: x86, x64 and missing-file checks remain in `PeBitness.DetectsBothArchitecturesAndRejectsMissingFile`; environment and positional arguments are retained.
- `j2534_bridge_integration_test`: open/connect/read, write success/failure, voltage and child-crash helpers are called with fatal-failure propagation from `J2534BridgeIntegration.CallsAndChildCrashContracts`.
- `j2534_bridge_client_test`: `J2534BridgeClient.OpensConnectsAndReadsThroughBridge`.
- `j2534_win_bridge_test`: `J2534WinBridge.OpensConnectsAndReadsThroughBridge`.

The legacy plural integration and force-asserts labels remain runnable test suites pointing to the Gazelle-named executables. The release force-asserts SIGABRT check is unchanged. Runtime resources, offscreen settings, platform constraints, x86 fixture transitions and Sonar exclusions are preserved.
