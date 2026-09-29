# QtTest migration baseline

Baseline commit: `67643874`. There are 44 QtTest executables; two existing GoogleTest files mention QtTest entry-point macros only in comments and are excluded from this count. Case names and data row names below must remain identifiable in the GoogleTest replacement. Helper methods are included pending manual classification.

## src/platform/desktop/common/service_functions/service_function_worker_test.cpp

- Methods: `submit`, `submit`, `build`, `initTestCase`, `completesWithoutEverRequestingAGate`, `appliesTheSessionsTransportConfigurationBeforeRunning`, `neverTouchesTheSerialFacadeWhenSetupFails`, `blocksOnAGateUntilItIsAnswered`, `aDeclinedGateReachesTheSessionAsDecline`, `requestStopUnblocksAnOutstandingGate`, `aStaleAnswerCannotSatisfyALaterGate`, `emitsFinishedExactlyOnceOnFailure`, `destructorDoesNotDestroyOwnedStateWhileResumeIsActive`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## src/platform/desktop/common/service_functions/serial_facade_configurator_test.cpp

- Methods: `isoConfigurationClearsAStaleKlineHeaderAndUsesTheRequiredOrder`, `klineConfigurationPreservesLegacyOpenBaudHeaderOrder`, `nullFacadeIsDisconnected`, `anEmptyOpenResultIsDisconnectedEvenWithAStaleOpenFlag`, `aPortThatIsNotOpenAfterOpenIsDisconnected`, `eachBooleanSetterFailureIsInvalidConfig_data`, `eachBooleanSetterFailureIsInvalidConfig`, `aKlineHeaderSetterFailureIsInvalidConfig`, `aSetterExceptionBecomesInternalStatus`, `anOpenExceptionBecomesInternalStatus`, `aRejectedBaudChangeIsInternal`, `aPortDropDuringRejectedBaudChangeIsDisconnected`, `aStandardFacadeExceptionBecomesInternalStatus`, `aNonStandardFacadeExceptionBecomesInternalStatus`
- Data rows: `set_is_iso14230_connection`, `set_is_can_connection`, `set_is_iso15765_connection`, `set_is_29_bit_id`, `set_add_iso14230_header`, `set_can_speed`, `set_iso15765_source_address`, `set_iso15765_destination_address`, `set_can_source_address`, `set_can_destination_address`
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/transport/desktop_logging_protocol_registration_test.cpp

- Methods: `expectCdbgSetup`, `registration_performs_no_io`, `cdbg_setup_failure_stops_at_failed_step`, `cdbg_open_failure`, `cdbg_success_preserves_start_sequence`, `ssm_target_and_adapter_are_per_run`, `ssm_snapshot_offsets_reach_samples`, `mut_dma_preserves_initialization_and_channels`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/transport/desktop_transport_factory_test.cpp

- Methods: `listsEveryDetectedPort`, `refusesToOpenWhenNoDeviceIsDetected`, `refusesToOpenWhenTheNamedDeviceIsAbsent`, `selectsTheFirstJ2534DeviceWhenNoNameIsGiven`, `skipsNonJ2534PortsWhenNoNameIsGiven`, `refusesToOpenWhenNoDetectedPortIsAJ2534Adapter`, `refusesToOpenWhenTheNamedDeviceIsNotAJ2534Adapter`, `reportsDisconnectedWhenTheOpenFails`, `refusesAConfigWithoutABackendFactory`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp

- Methods: `configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure`, `configureFailsAtEachRemainingSetterInTurn_data`, `configureFailsAtEachRemainingSetterInTurn`, `openFailureReturnsDisconnectedWithoutAnyWrite`, `configureSucceedsWhenEverySetterSucceeds`, `configureClearsStickyIso14230HeaderState`, `openSucceedsWhenBackendReturnsANonEmptyPortName`, `resetConnectionSucceedsAndReachesTheAdapter`, `restartIso15765ResetsConfiguresAndReopensInOrder`, `restartIso15765CancellationBeforeResetTouchesNoBackendOperation`, `restartIso15765ResetFailureStopsBeforeConfigurationOrOpen`, `restartIso15765ConfigureFailureStopsBeforeOpen`, `restartIso15765OpenFailurePropagatesAfterExactConfiguration`, `resetConnectionReturnsDisconnectedAfterClose`, `resetConnectionMapsStandardDriverExceptionsToInternal`, `resetConnectionMapsNonStandardDriverExceptionsToInternal`, `writeSucceedsWhenPortStaysOpenThroughout`, `writeReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingWrite`, `writeFailsWithDisconnectedWhenPortClosesDuringWrite`, `writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite`, `readReturnsScriptedBytesOnSuccess`, `readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead`, `readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead`, `readReturnsDisconnectedWhenPortClosesDuringRead`, `everyMethodFailsWithDisconnectedAfterClose`, `writeIsSkippedWithCancelledAfterRequestUnblock`, `readReturnsEmptyOptionalWhenBackendReturnsNoBytes`, `writeFailsWithInternalWhenDriverThrowsStandardException`, `writeFailsWithInternalWhenDriverThrowsNonStandardException`, `readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled`, `readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled`, `readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead`, `readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow`, `readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow`, `closeIsIdempotentAndDestroysTheOwnedSerialPortActions`, `closeOnANonOwningSerialPortActionsDoesNotDestroyIt`, `requestUnblockCausesAPendingReadToReturnPromptly`, `fakeBackendReportsScriptedPortListAndBattery`
- Data rows: `set_is_iso15765_connection`, `set_is_can_connection`, `set_is_iso14230_connection`, `set_is_29_bit_id`, `set_can_source_address`, `set_can_destination_address`, `set_iso15765_source_address`, `set_iso15765_destination_address`, `set_add_iso14230_header`
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/transport/desktop_kline_flash_transport_test.cpp

- Methods: `configureSetsAndResetsParityOnReusedFacade`, `configureReportsParitySetterFailure`, `rawCallsUseRawSerialMethods`, `postKernelUploadDelayCapabilityMirrorsOpenPort2OnUnix`, `programmingVoltageSupplyMirrorsOpenPort2OnEveryPlatform`, `resetConnectionReachesTheAdapter`, `resetConnectionAfterCloseIsDisconnectedAndTouchesNoBackend`, `lecControlOperationsForwardToSerialBackend`, `configureChecksEveryBooleanSetterInOrderAndStopsAtFirstFailure`, `configureFailsAtEachRemainingSetterInTurn_data`, `configureFailsAtEachRemainingSetterInTurn`, `openFailureReturnsDisconnectedWithoutAnyWrite`, `configureSucceedsWhenEverySetterSucceeds`, `openSucceedsWhenBackendReturnsANonEmptyPortName`, `setBaudSucceedsWhenPortOpenAndDriverReturnsSuccess`, `setBaudFailsWithInternalWhenPortStaysOpenButDriverRejectsChange`, `setBaudFailsWithDisconnectedWhenPortAlreadyClosed`, `setBaudFailsWithDisconnectedWhenPortClosesDuringBaudChange`, `writeSucceedsAndReturnsRequestedByteCount`, `writeFailsWithDisconnectedWhenPortClosesDuringWrite`, `readReturnsScriptedBytesOnSuccess`, `readReturnsCancelledWhenCancellationIsAlreadyObservedBeforeIssuingRead`, `readReturnsDisconnectedWhenPortAlreadyClosedBeforeRead`, `readReturnsDisconnectedWhenPortClosesDuringRead`, `everyMethodFailsWithDisconnectedAfterClose`, `setAddIso14230HeaderForwardsToSerialAndSucceeds`, `writeIsSkippedWithCancelledAfterRequestUnblock`, `writeFailsWithDisconnectedWhenPortAlreadyClosedBeforeWrite`, `readReturnsEmptyOptionalWhenBackendReturnsNoBytes`, `setBaudFailsWithInternalWhenDriverThrowsStandardException`, `setBaudFailsWithInternalWhenDriverThrowsNonStandardException`, `writeFailsWithInternalWhenDriverThrowsStandardException`, `writeFailsWithInternalWhenDriverThrowsNonStandardException`, `readFailsWithInternalWhenDriverThrowsStandardExceptionAndNotCancelled`, `readFailsWithInternalWhenDriverThrowsNonStandardExceptionAndNotCancelled`, `isOpenReflectsThePortsRealOpenState`, `isOpenReturnsFalseWhenTheUnderlyingCheckThrows`, `isOpenReturnsFalseAfterClose`, `readReturnsCancelledWhenCancellationBecomesObservedAfterASuccessfulRead`, `readReturnsCancelledWhenCancellationBecomesObservedDuringAStandardExceptionThrow`, `readReturnsCancelledWhenCancellationBecomesObservedDuringANonStandardExceptionThrow`, `closeIsIdempotentAndDestroysTheOwnedSerialPortActions`, `closeOnANonOwningSerialPortActionsDoesNotDestroyIt`, `requestUnblockCausesAPendingReadToReturnPromptly`
- Data rows: `set_is_iso14230_connection`, `set_is_can_connection`, `set_is_29_bit_id`, `set_serial_port_baudrate`
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/transport/desktop_mixed_can_flash_transport_test.cpp

- Methods: `configure_and_open`, `initialResetReachesBackendAndReturnsFailure`, `initialResetAfterCloseReturnsDisconnected`, `configuresIsoThenTransitionsRawAndBack`, `everyModeConfigurationClearsStickyIso14230HeaderState`, `preservesExtendedIsoIdDuringInitialConfigurationAndReturnTransition`, `rawFrameAddsAndParsesBigEndianId`, `configureFailsAtEverySetter`, `configureRejectsReconfigureWhileAlreadyConfigured`, `poisonedTransitionMakesConfigureAndOpenSurfaceTheStaleErrorEvenAfterClose`, `rawTransitionFailsAtEverySetterAndMakesIoTerminal`, `failedReopenMakesFollowingIoTerminal`, `rawReadRejectsShortFrameAndWrongReceiveId`, `clearReceiveBufferRejectsBackendFailure`, `detectsDisconnectionBeforeAndDuringIo`, `catchesStandardAndNonstandardBackendExceptions`, `cancellationAndUnblockSuppressSubsequentIo`, `nonOwningCloseDoesNotDestroyCallerSerial`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## src/platform/desktop/common/connection/adapter_connection_test.cpp

- Methods: `parsesTheToolbarTransportText`, `listsPortsFromTheFacade`, `setInitialPortSetsTheBaudThenThePort`, `selectPortReplacesThePortList`, `openAndIsOpenAskTheFacade`, `openedPortAsksTheFacade`, `canTransportIsRawCanElevenBit`, `iso15765TransportIsTwentyNineBit`, `klineWithSsmRunsAtFourThousandEightHundred`, `klineWithoutSsmLeavesTheSpeedAlone`, `clearLinkFlagsClearsEveryFlagAndKeepsParity`, `returnToIdleResetsBaudAndParityAndKeepsTheFlags`, `setPortSpeedPassesTheBaudAsText`, `batteryIsReadOnlyFromAnOpenPort`, `waitForSourceWaitsOnTheFacade`, `forwardsFacadeStateChanges`, `exposesTheSameFacade`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/flash/flash_workflow_test.cpp

- Methods: `expectCanTransportSetup`, `expectNoBackendIo`, `recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone`, `invalidColtSuffixIsRecognizedButFailsPreflight`, `preflightPrecedesPromptsAndDeclineCancels`, `successfulReadBytesAreAcceptedAutomatically`, `subaruMitsuPropagatesRomId`, `subaruHitachiRoutesBothModesAndPropagatesReadResult`, `routesTcuHitachiM32rKlineReadOnly`, `unisiaJecsRoutesOnlyExactProtocolMcuPairs`, `unisiaJecsCrossPairsFailBeforeAttempt`, `routesTcuHitachiM32rCanReadAndWriteRejectsTestWrite`, `routesSh72543rAliasesAndPreservesImageAndIdentity`, `routesSh7058ReadAndWriteWithPreTransportPrompts`, `sh72543rRejectsPreflightAndDeclinedBegin`, `sh72543rPropagatesFailureAndAbsentIdentity`, `coltWriteUsesColtSpecificSafetyPrompts`, `mc68BdmReadRoutesThroughBeginToAttempt`, `mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom`, `mc68BdmDeclinedBootstrapConfirmationCancels`, `mc68BdmDeclinedBeginCancels`, `mc68BdmTestWriteFailsBeforeAnyPrompt`, `mc68BdmPrefixLookalikeStaysOffTheKlineFamily`, `mc68TpuProtocolIsClaimedByPortableRoute`, `mc68Revision04IsClaimedButPlanBuildFails`, `sh7055ProtocolIsClaimedByPortableRoute`, `densoCanRoutesOnlyTheFiveExactProtocols`, `densoCanResolvesKernelPromptsAndPropagatesAttemptResult`, `densoCanPreflightAndDeclinedPromptsStopBeforeAttempt`, `petrolRoutesOnlyTheFiveExactProtocols`, `petrolSupportedOperationsResolveSecurityAndCatalogKernel`, `petrolSuccessfulReadPropagatesBytesAndRomId`, `petrolReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport`, `dieselRoutesOnlyTheTwoExactProtocols`, `dieselSupportedOperationsResolveGenerationCatalogKernels`, `dieselSuccessfulReadPropagatesKernelSnapshotBytesAndRomId`, `dieselReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport`, `tcuRoutesOnlyTheTwoExactProtocols`, `tcuSupportedOperationsResolveTheirCatalogKernelAndReachAttempt`, `tcuUnsupportedOperationsFailBeforeTransportIo`, `tcuReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport`, `tcuSuccessfulReadPropagatesBytesAndRomId`, `mc68ResolvesKernelThroughCatalogBeforePromptAndAttempt`, `missingCatalogKernelFailsBeforePrompt`, `sh7055IteratesConfirmationsAndPropagatesAttemptResult`, `sh7055EcutekResolvesWithoutCarModelReference`, `portableImageCopiesRomForEveryNonReadOperation`, `mc68TestWriteWithPortableImageReachesAttempt`, `mc68PhysicalImageIsPackedAtWorkflowBoundary`, `mc68CalibrationPaddingRoundTripsToPackedWriteImage`, `sh7055TestWriteWithPortableImageReachesPromptsAndAttempt`, `mc68TpuReadResolvesCatalogAndReachesAttempt`, `densoSh705xKlineRoutesExactProtocolsThroughBeginToAttempt`, `densoSh705xKlineCobbReadFailsBeforeAttempt`, `densoSh705xKlineIgnoresPrefixLookalikes`, `unisiaJecsM32rRoutesTheFourExactProtocols`, `unisiaJecsM32rLookalikesStayUnrouted`, `unisiaBootmodeReadUsesTheKlineReadFamily`, `unisiaBootmodeWriteRunsKernelThenMod1ThenProgram`, `unisiaBootmodeKernelFailureSkipsMod1AndProgram`, `unisiaBootmodeKernelCancelledShowsNotice`, `unisiaBootmodeDeclinedMod1CancelsWithNotice`, `unisiaBootmodeProgramFailureShowsNoticeThenFailure`, `unisiaBootmodeDeclinedVoltagesCancelsBeforeAnyAttempt`, `unisiaBootmodeWrongImageSizeFailsBeforeAnyPrompt`, `unisiaBootmodeMissingKernelFailsBeforeAnyPrompt`, `unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt`, `unisiaBootmodeTestWriteIsUnsupported`, `unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter`, `unisiaJecsM32rFailedWriteRemindsBeforeReportingTheFailure`, `unisiaJecsM32rCancelledWriteReminds`, `unisiaJecsM32rDeclinedVppPromptCancelsBeforeAttempt`, `unisiaJecsM32rAdapterSuppliedVppSkipsBothPrompts`, `unisiaJecsM32rAdapterSuppliedVppFailedWriteWarnsNotToPowerOff`, `unisiaJecsM32rAdapterSuppliedVppCancelledWriteWarnsNotToPowerOff`, `unisiaJecsM32rReadPropagatesRomIdWithoutVppPrompts`, `unisiaJecsM32rFailedReadReportsWithoutNotice`, `unisiaJecsM32rWriteOnReadOnlyVariantFailsBeforeAnyPrompt`, `recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone`, `invalidColtSuffixIsRecognizedButFailsPreflight`, `preflightPrecedesPromptsAndDeclineCancels`, `successfulReadBytesAreAcceptedAutomatically`, `unisiaJecsRoutesOnlyExactProtocolMcuPairs`, `unisiaJecsCrossPairsFailBeforeAttempt`, `subaruMitsuPropagatesRomId`, `subaruHitachiRoutesBothModesAndPropagatesReadResult`, `routesTcuHitachiM32rKlineReadOnly`, `routesTcuHitachiM32rCanReadAndWriteRejectsTestWrite`, `routesSh72543rAliasesAndPreservesImageAndIdentity`, `routesSh7058ReadAndWriteWithPreTransportPrompts`, `sh72543rRejectsPreflightAndDeclinedBegin`, `sh72543rPropagatesFailureAndAbsentIdentity`, `coltWriteUsesColtSpecificSafetyPrompts`, `mc68BdmReadRoutesThroughBeginToAttempt`, `mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom`, `mc68BdmDeclinedBootstrapConfirmationCancels`, `mc68BdmDeclinedBeginCancels`, `mc68BdmTestWriteFailsBeforeAnyPrompt`, `mc68BdmPrefixLookalikeStaysOffTheKlineFamily`, `mc68TpuProtocolIsClaimedByPortableRoute`, `mc68Revision04IsClaimedButPlanBuildFails`, `sh7055ProtocolIsClaimedByPortableRoute`, `densoCanRoutesOnlyTheFiveExactProtocols`, `densoCanResolvesKernelPromptsAndPropagatesAttemptResult`, `densoCanPreflightAndDeclinedPromptsStopBeforeAttempt`, `petrolRoutesOnlyTheFiveExactProtocols`, `petrolSupportedOperationsResolveSecurityAndCatalogKernel`, `petrolSuccessfulReadPropagatesBytesAndRomId`, `petrolReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport`, `dieselRoutesOnlyTheTwoExactProtocols`, `dieselSupportedOperationsResolveGenerationCatalogKernels`, `dieselSuccessfulReadPropagatesKernelSnapshotBytesAndRomId`, `dieselReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport`, `tcuRoutesOnlyTheTwoExactProtocols`, `tcuSupportedOperationsResolveTheirCatalogKernelAndReachAttempt`, `tcuUnsupportedOperationsFailBeforeTransportIo`, `tcuReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport`, `tcuSuccessfulReadPropagatesBytesAndRomId`, `mc68ResolvesKernelThroughCatalogBeforePromptAndAttempt`, `missingCatalogKernelFailsBeforePrompt`, `sh7055IteratesConfirmationsAndPropagatesAttemptResult`, `sh7055EcutekResolvesWithoutCarModelReference`, `portableImageCopiesRomForEveryNonReadOperation`, `mc68TestWriteWithPortableImageReachesAttempt`, `mc68PhysicalImageIsPackedAtWorkflowBoundary`, `mc68CalibrationPaddingRoundTripsToPackedWriteImage`, `sh7055TestWriteWithPortableImageReachesPromptsAndAttempt`, `mc68TpuReadResolvesCatalogAndReachesAttempt`, `densoSh705xKlineRoutesExactProtocolsThroughBeginToAttempt`, `densoSh705xKlineCobbReadFailsBeforeAttempt`, `densoSh705xKlineIgnoresPrefixLookalikes`, `unisiaJecsM32rRoutesTheFourExactProtocols`, `unisiaJecsM32rLookalikesStayUnrouted`, `unisiaBootmodeReadUsesTheKlineReadFamily`, `unisiaBootmodeWriteRunsKernelThenMod1ThenProgram`, `unisiaBootmodeKernelFailureSkipsMod1AndProgram`, `unisiaBootmodeKernelCancelledShowsNotice`, `unisiaBootmodeDeclinedMod1CancelsWithNotice`, `unisiaBootmodeProgramFailureShowsNoticeThenFailure`, `unisiaBootmodeDeclinedVoltagesCancelsBeforeAnyAttempt`, `unisiaBootmodeWrongImageSizeFailsBeforeAnyPrompt`, `unisiaBootmodeMissingKernelFailsBeforeAnyPrompt`, `unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt`, `unisiaBootmodeTestWriteIsUnsupported`, `unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter`, `unisiaJecsM32rFailedWriteRemindsBeforeReportingTheFailure`, `unisiaJecsM32rCancelledWriteReminds`, `unisiaJecsM32rDeclinedVppPromptCancelsBeforeAttempt`, `unisiaJecsM32rAdapterSuppliedVppSkipsBothPrompts`, `unisiaJecsM32rAdapterSuppliedVppFailedWriteWarnsNotToPowerOff`, `unisiaJecsM32rAdapterSuppliedVppCancelledWriteWarnsNotToPowerOff`, `unisiaJecsM32rReadPropagatesRomIdWithoutVppPrompts`, `unisiaJecsM32rFailedReadReportsWithoutNotice`, `unisiaJecsM32rWriteOnReadOnlyVariantFailsBeforeAnyPrompt`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## src/platform/desktop/common/flash/flash_worker_test.cpp

- Methods: `request_unblock`, `closingWhileReadIsBlocked_cancelsUnblocksAndJoinsWithoutWallClockSleep`, `oneAndOnlyOneTerminalResultIsEmitted`, `phaseProgressIsForwardedAlongsideLegacyProgress`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## src/platform/desktop/common/diagnostics/ssm_identify_worker_test.cpp

- Methods: `stopsAtTheFirstSuccess`, `retriesFiveTimesThenReportsTheLastError`, `stopBeforeStartCancelsAfterOneAttempt`, `destroyingARunningWorkerJoinsIt`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## src/platform/desktop/common/diagnostics/serial_diagnostic_link_test.cpp

- Methods: `klineOpenResetsAppliesEverySetterThenOpens`, `canOpenResetsAppliesEverySetterThenOpens`, `evenParityIsAppliedBeforeTheOpen`, `failingSetterIsInvalidConfigAndStopsTheSequence`, `emptyOpenedPortIsDisconnected`, `setHeaderSetsAllThreeFlags`, `p1UsesTheJ2534IoctlOnOpenPort`, `p1UsesKlineTimingsOnDirectSerial`, `initCallsPassBytesThrough`, `writeIsEchoCheckedAndReadsSelectTheFacadeCall`, `cancelledReadNeverReachesTheFacade`, `nullFacadeIsDisconnected`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/diagnostics/dtc_worker_test.cpp

- Methods: `reportsTheSessionOutcomeAndForwardsLogLines`, `stopBeforeStartCancelsTheRun`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## src/platform/desktop/common/logging/logging_worker_test.cpp

- Methods: `log`, `progress`, `notice`, `forwards_portable_states_samples_and_cancelled_result`, `forwards_final_start_error_without_policy_mapping`, `destruction_cancels_and_joins_a_blocked_poll`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## src/platform/desktop/common/logging/logging_engine_test.cpp

- Methods: `releaseFailure`, `expect_start_error`, `start_rejections_data`, `start_rejections`, `user_stop_publishes_joined_completion_exactly_once`, `completion_observer_can_immediately_start_a_second_run`, `explicit_stop_restart_ignores_stale_worker_events_and_preserves_handshake_classification`, `natural_terminal_result_is_published_once_after_reprocessing_queued_delivery`, `successful_worker_result_is_reported_as_runtime_failure`, `destruction_joins_blocked_run_without_publishing_completion`, `every_cdbg_serial_setup_failure_is_structured_and_stops_before_later_steps`, `start_error_preserves_handshake_failure_ui_path`, `disconnect_error_preserves_adapter_failure_ui_path`, `post_start_failure_is_not_reported_as_handshake_failure`, `unexpected_cancelled_outcome_is_reported_as_runtime_failure`, `diagnostic_slot_forwards_error_level_with_timestamp_and_linefeed`, `diagnostic_slot_forwards_warning_level_with_timestamp_and_linefeed`, `diagnostic_slot_forwards_info_level_with_timestamp_and_linefeed`, `diagnostic_slot_forwards_debug_level_with_timestamp_and_linefeed`, `portable_events_map_to_existing_status_and_value_signals`
- Data rows: `active run`, `unknown ID`, `null factory value`, `returned error`, `std exception`, `unknown exception`
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## src/platform/desktop/common/serial/facade_threading_test.cpp

- Methods: `constructDestroy_withoutUse_noThreadNoHang`, `getSet_marshalsToBackendThread`, `scriptedRead_returnsThroughFacade`, `backendException_propagatesWithoutHangingAndCleansUp`, `transportAdapters_isOpenContainsBackendException`, `transportAdapters_normalEmptyReadIsSuccess`, `transportAdapters_preCancelledReadSkipsBackend`, `transportAdapters_postCallCancellationPrecedesDisconnect`, `transportAdapters_backendReadExceptionMapsToInternal`, `canTransport_truncatedFrameMapsToInternal`, `transportAdapters_nullOrClosedAdapterReturnsDisconnectedBeforeOperation`, `transportAdapters_writeSuccessAndCanFrameEncoding`, `transportAdapters_disconnectDuringWriteMapsToDisconnected`, `transportAdapters_disconnectDuringReadMapsToDisconnected`, `transportAdapters_backendWriteExceptionMapsToInternal`, `transportAdapters_backendNonStandardExceptionMapsToInternal`, `transportAdapters_cancellationPrecedesReadException`, `klineTransport_setBaudSuccessRejectionDisconnectException`, `workerThreadCaller_noAffinityWarnings`, `concurrentCallers_serializeWithoutInterleaving`, `destroyAfterUse_joinsIoThread`, `destroyWhileReadInFlight_waitsForBackendCall`, `constructDestroy_withoutUse_noThreadNoHang`, `getSet_marshalsToBackendThread`, `scriptedRead_returnsThroughFacade`, `backendException_propagatesWithoutHangingAndCleansUp`, `transportAdapters_isOpenContainsBackendException`, `transportAdapters_normalEmptyReadIsSuccess`, `transportAdapters_preCancelledReadSkipsBackend`, `transportAdapters_postCallCancellationPrecedesDisconnect`, `transportAdapters_backendReadExceptionMapsToInternal`, `canTransport_truncatedFrameMapsToInternal`, `transportAdapters_nullOrClosedAdapterReturnsDisconnectedBeforeOperation`, `transportAdapters_writeSuccessAndCanFrameEncoding`, `transportAdapters_disconnectDuringWriteMapsToDisconnected`, `transportAdapters_disconnectDuringReadMapsToDisconnected`, `transportAdapters_backendWriteExceptionMapsToInternal`, `transportAdapters_backendNonStandardExceptionMapsToInternal`, `transportAdapters_cancellationPrecedesReadException`, `klineTransport_setBaudSuccessRejectionDisconnectException`, `warningCapture`, `workerThreadCaller_noAffinityWarnings`, `concurrentCallers_serializeWithoutInterleaving`, `destroyAfterUse_joinsIoThread`, `destroyWhileReadInFlight_waitsForBackendCall`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/serial/remote_backend_smoke_test.cpp

- Methods: `constructAndDestroy_localPeer_noBlockNoCrash`, `constructAndDestroy_localPeer_noBlockNoCrash`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/serial/direct_backend_hooks_windows_test.cpp

- Methods: `resolvePort_keepsTheVendorNameWhole`, `resolvePort_emptyEntryIsNotJ2534`, `txDone_isAlwaysTrue`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## src/platform/desktop/common/serial/j2534_driver_selection_windows_test.cpp

- Methods: `capableEntry_acceptsEveryNonEmptyEntry`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## src/platform/desktop/common/serial/serial_idle_test.cpp

- Methods: `resetsTheConnectionThenRestoresTheIdleLineSettingsInOrder`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/serial/direct_backend_test.cpp

- Methods: `getSet_roundtrip_throughInterface`, `closedPort_ioCalls_returnEmpty`, `j2534Selection_usesInstalledDllPathAfterVendorProbe`, `j2534DriverViews_wow6432NodeVendorIsDiscoverable`, `j2534DriverViews_laterViewOverwritesOnCollision`, `makeDirectSerialBackend_buildsTheDirectBackend`, `getSet_roundtrip_throughInterface`, `closedPort_ioCalls_returnEmpty`, `j2534Selection_usesInstalledDllPathAfterVendorProbe`, `j2534DriverViews_wow6432NodeVendorIsDiscoverable`, `j2534DriverViews_laterViewOverwritesOnCollision`, `makeDirectSerialBackend_buildsTheDirectBackend`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/serial/direct_backend_hooks_unix_test.cpp

- Methods: `resolvePort_prefixesAndSplitsAnAdapterEntry`, `resolvePort_plainSerialEntryIsNotJ2534`, `appendJ2534Interfaces_leavesTheListUntouched`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## src/platform/desktop/common/serial/j2534_driver_selection_unix_test.cpp

- Methods: `capableEntry_matchesOnlyTheAdapterDescription`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## src/platform/desktop/common/serial/direct_backend_pty_test.cpp

- Methods: `initTestCase`, `ptyRead_reassemblesFragmentedFrame`, `ptyRead_timesOutCleanOnSilence`, `ptyClearRxBuffer_discardsPendingBytes`, `ptyAdapterVanish_readReturnsCleanly`, `ptyParityChangesWhileOpen`, `initTestCase`, `ptyRead_reassemblesFragmentedFrame`, `ptyRead_timesOutCleanOnSilence`, `ptyClearRxBuffer_discardsPendingBytes`, `ptyAdapterVanish_readReturnsCleanly`, `ptyParityChangesWhileOpen`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/serial/desktop_serial_factory_test.cpp

- Methods: `log_messages`, `directConnectionBuildsTheDirectBackend`, `remoteConnectionBuildsTheRemoteBackend`, `everyLogLevelReachesTheSink`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## src/platform/desktop/common/serial/testing/fake_backend_test.cpp

- Methods: `defaultActionsPreserveConfigurationThroughFacade`, `expectationsScriptFacadeIoInOrder`, `expectationFailuresProduceNonzeroExit`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/platform/desktop/common/serial/testing/fake_backed_serial_test.cpp

- Methods: `theBackendIsLiveAsSoonAsTheFixtureIsConstructed`, `arrangeRunsBeforeTheFixtureTouchesTheBackend`, `releaseTransfersTheFacadeAndLeavesTheFakeReachable`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/ui/desktop/settings_test.cpp

- Methods: `providerCheckboxesPersistBothDirections_data`, `providerCheckboxesPersistBothDirections`, `removingDefinitionsPreservesOrderAndPersistsEmptyList_data`, `removingDefinitionsPreservesOrderAndPersistsEmptyList`, `closingSettingsSavesThroughTheSession`, `editsReachTheSessionLive`, `destructionRetriesPersistenceAfterClose`, `failedSaveKeepsEditsAndWarnsTheOperator`
- Data rows: `enabled-romraider`, `disabled-ecuflash`, `surviving-order`, `empty-list`
- Conditional skip sites: 0
- Entry point: MAIN

## src/ui/desktop/dtc_operations_test.cpp

- Methods: `aFailedRunLogsOnceAndReenablesTheButtons`, `closeDuringARunStopsTheWorkerAndResets`, `escapeDuringARunStopsTheWorkerAndResets`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## src/ui/desktop/vehicle_select_test.cpp

- Methods: `choosingRecordsTheRowWithoutTouchingTheSession`, `rejectingLeavesNoChoice`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## src/ui/desktop/mainwindow_test.cpp

- Methods: `start`, `stop`, `drive`, `initTestCase`, `explicitConfigRootLoadsFixtureAndProvisionsDirectories`, `directSessionStartupNeverWaitsForARemoteSource`, `windowLogLinesReachTheLogChannel`, `windowEnablesFileLoggingThroughTheChannel`, `directSessionStartupNeverRequestsTheRemoteWait`, `externalLoggerMirrorsToTheRemotePeer`, `peerStateChangesReachTheWindow`, `handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling_data`, `handledDensoTcuReadChoicesRunMainWindowCleanupAndStopVoltagePolling`, `futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo_data`, `futureDensoSuffixesDoNotInstantiateKlineOrPerformEcuIo`, `representativePortableRoutesReachFactoryBeforeLegacyFallback_data`, `representativePortableRoutesReachFactoryBeforeLegacyFallback`, `writeWithoutASelectedCalibrationStopsVoltagePolling`, `otherMakesSkipDispatchButStillRunCleanup`, `readOfAnUnsupportedProtocolAddsNoCalibration`, `cancellingTheChecksumWarningStopsVoltagePolling`, `definitionlessOpenPromptsOnceAndAppliesPlaceholders`, `closingAMiddleRomKeepsLaterRomsAddressable`, `windowsOfAClosedRomAreInert`, `hexEditorOutlivesItsRom`, `closingARomClosesAllOfItsWindows`, `viewStateIsKeptPerRom`, `writeMetadataFillsAnEmptyDefinitionFlashMethod`, `writeMetadataLeavesADefinitionlessFlashMethodAlone`, `checksumAndSaveUseATemporaryImage`, `saveAsChangesSourceAndTreeOnlyAfterSuccess`, `selectableSignalEditsItsEmittingSession`, `failedMapDecodeDoesNotOccupyAView`, `windowPreservesInjectedLoggingFactory`, `loggingCapturesTargetForEachRun`, `chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation_data`, `chooserDialogsApplyAcceptedChoicesAndIgnoreCancellation`, `definitionManagerRemovesSelectedRowsAndSavesSurvivingOrder`, `numericWindowGeometryRestoresAndPersistsAcrossWindowStates`, `acceptedVehicleChoiceSelectsTheRowAndSavesIt`, `cancelledVehicleChoiceChangesNothing`, `acceptedProtocolChoiceSelectsTheLastMatchingRow`, `romFlashMethodSelectsTheLastMatchingRow`, `unmatchedRomFlashMethodChangesNothing`, `unresolvedProtocolRowLeavesReadAndWriteUnavailable`, `loggingUsesTheSessionLogProtocol`, `selectedSerialPortIsEmptyWithoutPorts`, `dtcWindowWithoutAPortWarnsInsteadOfCrashing`, `repeatedSaveFailuresLogOnceUntilASuccess`, `biuWindowRemembersTheOpenedPort`, `disconnectReturnsTheAdapterToIdle`, `connectOnAnotherMakeDisconnectsWithoutIdentifying`, `subaruKlineConnectIdentifiesOffTheUiThread`, `subaruConnectThatNeverAnswersDisconnectsAndRestoresControls`, `disconnectDuringIdentificationCancelsAndDropsTheResult`, `loggingStartWaitsForIdentification_data`, `loggingSelectionFailureSemanticsAndSupportPreservation`, `loggingDefinitionFailureIsNonfatal`, `unresolvedDisplaySlotsAreSkippedAndUpdateTheirOriginalLabels`, `chooserDuplicateLabelIdentity_data`, `chooserDuplicateLabelIdentity`, `csvSharedIdProtocolIdentity`, `loggingStartWaitsForIdentification`, `batterySamplingDoesNotUseTheFacadeDuringIdentification`, `windowDestructionJoinsIdentificationWithoutContinuingLogging`, `connectStopsAnActiveLoggingWorkerBeforeIdentification`, `connectionEntryPointsStopIdentification_data`, `connectionEntryPointsStopIdentification`, `nestedConnectDuringCapabilityNoticeKeepsEachContinuation`, `copyFixtureConfig`, `selectProtocol`, `selectSubaruProtocol`, `selectMake`, `prepareConnect`, `installLoggingFixture`
- Data rows: `chooser-cancelled`, `relearn-declined`, `future-can`, `extra-densocan`, `petrol`, `densocan`, `denso_sh705x_kline`, `vehicle-accept`, `vehicle-cancel`, `protocol-accept`, `protocol-cancel`, `ECU`, `TCU`, `gauge`, `digital`, `switch`
- Conditional skip sites: 0
- Entry point: custom main

## src/ui/desktop/protocol_select_test.cpp

- Methods: `listsEachVehicleBackedProtocolOnce`, `choosingRecordsTheProtocolName`, `rejectingLeavesNoChoice`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## src/ui/desktop/calibration_maps_test.cpp

- Methods: `replace_definition`, `layoutsAndRefresh_data`, `layoutsAndRefresh`, `staticAxisLabels_data`, `staticAxisLabels`, `absentAxisUsesSequentialFallback`, `selectableReflectsBlobBytesWithoutEmittingEditSignal`, `retainedMultiSelectableGeometryKeepsLegacyNumericCell`, `retainedSwitchRefreshKeepsUncheckedControlWithoutEmittingEdits`, `colorsKeepOpeningBoundsDuringRefreshAndReopenUsesCurrentValues`, `constantMapHasFiniteStableColors`, `closedSessionRefreshIsInertAfterAnotherSessionOpens`
- Data rows: `1D`, `X 2D`, `Y 2D`, `3D`, `static X`, `static Y`
- Conditional skip sites: 0
- Entry point: MAIN

## src/ui/desktop/calibration_treewidget_test.cpp

- Methods: `filesTreeCarriesNameFirstMapIdAndSessionKey`, `dataTreeMatchesLegacyRules`, `definitionlessRomShowsOnlyRomInfo`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## src/ui/desktop/service_functions/denso_tcu_read_preflight_test.cpp

- Methods: `start`, `drive`, `start`, `drive`, `expectNoBackendIo`, `chooserReturnsTheActionNamedByEachLegacyButton_data`, `chooserReturnsTheActionNamedByEachLegacyButton`, `dismissingChooserReturnsCancelled`, `dumpAndCancelledReturnWithoutIgnitionOrSerialCalls_data`, `dumpAndCancelledReturnWithoutIgnitionOrSerialCalls`, `decliningIgnitionSkipsEveryServiceDialogAndSerialCall_data`, `decliningIgnitionSkipsEveryServiceDialogAndSerialCall`, `acceptingIgnitionOpensTheMatchingRealServiceDialog_data`, `acceptingIgnitionOpensTheMatchingRealServiceDialog`
- Data rows: `dump`, `relearn`, `read`, `set`, `dump`, `cancelled`, `relearn`, `read`, `set`, `relearn`, `read`, `set`
- Conditional skip sites: 0
- Entry point: MAIN

## src/ui/desktop/service_functions/service_function_dialog_test.cpp

- Methods: `setParametersSpinBoxesCarryTheLegacyPromptBounds`, `everyFormFieldLandsInItsOwnStructMember`, `setParametersFormIsOneDialogNotNineModals`, `readParametersRendersAllNineLegacyQualifiedLabelsAndValues`, `readParametersHasNoSpinBoxes`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## src/ui/desktop/flash/operation/flash_operation_controller_test.cpp

- Methods: `drive`, `expectNoEcuIo`, `unknownProtocolIsUnsupportedAndWarnsWithoutSerialIo`, `cancelledDensoTcuChooserIsHandledWithoutSerialIo`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## src/ui/desktop/flash/common/flash_dialog_test.cpp

- Methods: `submit`, `submit`, `request_unblock`, `request_unblock`, `submit`, `submit`, `submit`, `submit`, `showSuccess`, `showFailure`, `returnsAcceptedBytesAndUsesNormalizedReadTitle`, `closingMidAttemptSubmitsCancelledAndPresentsTheNotice`, `runsASecondAttemptAfterAPromptBetweenAttempts`, `programmingVoltageNoticeKeepsTheSixC3AdviceByDefault`, `programmingVoltageNoticeWithoutPowerOffAdviceOnFailure`, `programmingVoltageNoticeWithoutPowerOffAdviceOnSuccess`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: MAIN

## tests/serial_pty_e2e_test.cpp

- Methods: `workerThread_writeRead_overPty_deliversFramedMessage`, `workerThread_writeRead_overPty_deliversFramedMessage`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## tests/tst_serial_port_crash.cpp

- Methods: `detachSerialPort`, `installNullSerialJ2534`, `deleteAndNullJ2534`, `isSerialPortOpen_withNullSerial_doesNotCrash`, `readSerialData_withNullSerial_doesNotCrash`, `readSerialData_withNullSerial_doesNotBusySpin`, `passThruReadMsgs_withNullSerial_doesNotCrash`, `readVbatt_throughNullJ2534Serial_doesNotCrash`, `reentrantReadDuringTeardown_viaEventLoop_doesNotCrash`, `j2534Handshake_overMockPty_readVersionSucceeds`, `spadInitJ2534Connection_overMockPty_succeeds`, `loggingFlow_connectReadTeardownReentrancy_overMockPty_doesNotCrash`, `resetQueuedDuringRead_runsAfterReadCompletes`, `blockingRead_doesNotDispatchQueuedEvents`, `isSerialPortOpen_withNullSerial_doesNotCrash`, `readSerialData_withNullSerial_doesNotCrash`, `readSerialData_withNullSerial_doesNotBusySpin`, `passThruReadMsgs_withNullSerial_doesNotCrash`, `readVbatt_throughNullJ2534Serial_doesNotCrash`, `reentrantReadDuringTeardown_viaEventLoop_doesNotCrash`, `j2534Handshake_overMockPty_readVersionSucceeds`, `spadInitJ2534Connection_overMockPty_succeeds`, `loggingFlow_connectReadTeardownReentrancy_overMockPty_doesNotCrash`, `resetQueuedDuringRead_runsAfterReadCompletes`, `blockingRead_doesNotDispatchQueuedEvents`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## tests/tst_mut_dma_integration.cpp

- Methods: `resetParser`, `injectDataFrame`, `onReadable`, `process`, `handleLine`, `reply`, `run`, `connectsOverMockPty_facadeReportsOpen`, `setBaud_throughAdapter_trueWhenConnected_falseWhenClosed`, `write_throughAdapter_putsExactFrameOnWire`, `read_throughAdapter_returnsEcuReplyBytes`, `driverPollOnce_throughAdapter_decodesStreamFrameFromWire`, `connectsOverMockPty_facadeReportsOpen`, `setBaud_throughAdapter_trueWhenConnected_falseWhenClosed`, `write_throughAdapter_putsExactFrameOnWire`, `read_throughAdapter_returnsEcuReplyBytes`, `driverPollOnce_throughAdapter_decodesStreamFrameFromWire`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: GUILESS_MAIN

## tests/force_asserts/tst_force_asserts.cpp

- Methods: `outOfBoundsAtAborts`, `outOfBoundsAtAborts`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: APPLESS_MAIN

## apps/desktop/startup_diagnostics_test.cpp

- Methods: `handler`, `realPresentersReportSeverityAndOrderedDetails`, `emptyWarningsDoNotOpenAModalOrLog`, `sinkRetainsBoundedUtf8DiagnosticsInOrder`, `failureTextCarriesTheDetail`, `warningTextListsEveryWarning`, `defaultRootIsUnderHomeAndEndsInFastEcu`
- Data rows: none / dynamically generated
- Conditional skip sites: 0
- Entry point: custom main

## apps/desktop/desktop_composition_test.cpp

- Methods: `release`, `compositionRegistersAllLoggingProtocolsWithoutWindow`, `servicesReferToTheCompositionsOwnObjects`, `failedStartupBuildsNoServicesAndPerformsNoEcuIo`, `workspaceOpensARomFromDisk`, `migrationLoadsPreviousVersionSettingsFromDisk`, `malformedSettingsRejectStartup`, `settingsRewriteFailureIsAStartupWarning`, `servicesShareTheCompositionsSession`, `restartSeesSavedSettingsButNotTheDatalogDirectory`, `waitRequestIsWiredToTheRemoteUtility`, `remoteStateChangesReachThePeer`, `mirroringWithoutAPeerReturnsPromptly`, `channelLevelsReachTheLogWindowWithTheirPrefix`, `debugLinesStayOutOfTheLogWindow`, `relayedLineSurvivesItsSenderButADirectOneDoesNot`, `enablingFileLoggingWritesASyslogFile`, `destructionRightAfterConstructionDoesNotHang`, `constructingTwiceInOneProcessSucceeds`, `emptyHostSelectsTheDirectBackend`, `nonEmptyHostSelectsTheRemoteBackendWithItsCredentials`
- Data rows: none / dynamically generated
- Conditional skip sites: 1
- Entry point: GUILESS_MAIN
