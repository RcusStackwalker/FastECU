# Desktop Logging Run Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move desktop logging run state and lifecycle ordering into one window-owned coordinator while preserving the existing logging engine, connection flow, displays, CSV behavior, and hardware policy.

**Architecture:** Add `DesktopLoggingRun` beside the window in the existing UI widgets package. It borrows `LoggingEngine`, owns the captured snapshot, formatted-value cache, elapsed timer, CSV file, and CSV failure state, and returns a copied presentation view. `MainWindow` continues preparing run inputs after identification, routes start/stop/CSV actions through the coordinator, and renders its view.

**Tech Stack:** C++23, Qt 6, Bazel/Gazelle, GoogleTest, existing `LoggingEngine`, desktop logging adapters, scripted protocol and failing CSV storage fixtures.

**Spec:** [Desktop logging run ownership](../specs/2026-10-13-desktop-logging-run-ownership.md)

## Global Constraints

- “Keep the module desktop-side. It may use Qt and existing desktop adapters; portable backend policy gains no Qt, thread or filesystem dependencies.”
- “Composition retains ownership of the engine, worker execution resources and borrowed hardware dependencies. The module borrows the engine and stops its run before those dependencies are released.”
- “Route all desktop acquisition start/stop requests through the module, including Connect and window destruction. Engine internals continue to own worker cancellation, joining and queued-event generation fencing.”
- “Own active snapshot, value cache, elapsed run timing, CSV file, CSV failure latch and output path. CSV enablement persists between successive runs.”
- “Keep the pending start request and identification sequencing with the window's connection flow. After identification succeeds, that flow supplies captured run inputs. The module does not own connection decisions.”
- “Keep logger definitions, selection editing/persistence, idle display choices, widget layout, translation, OFF/ON rendering and dialogs with existing owners.”
- “Publish a coherent read-only view of active selection, measurement metadata and readings. Callers must not coordinate separately mutable snapshot/cache state.” Return that view by value; retain no borrowed view across notifications.
- “Finish owned state transitions before publishing their outcomes. Synchronous subscribers may re-enter; do not assume that notification delivery prevents this. Engine startup can emit diagnostics before returning an error, and Stop publishes completion synchronously after joining and releasing its run.”
- “Preserve state initialization before acquisition startup and rollback on a returned startup failure. A CSV failure alone must not block acquisition.”
- “Preserve per-sample stop checks and healthy-peer handling after a sample error. Characterize replacement of a run during notification separately.”
- “Preserve fresh-file creation, captured columns, first sample retention and once-per-failure reporting. Explicit CSV re-enable and a new run retry; ordinary subsequent samples do not retry after a latched failure.”
- “Elapsed time resets per logging run, not per CSV re-enable. Shutdown closes output and reports close errors without displaying modal notices.”
- “Keep identification shutdown ordering and synchronous logging cleanup before connection/facade reuse. Normal cleanup publishes completion only after owned run state and output have been released.”
- “Prefer existing seams and adapters. Production protocols and scripted protocols already vary; CSV uses real and failing storage adapters. Do not invent a virtual engine or generic storage interface solely for extraction.”
- Repository workflow: “Use a feature branch and pull request; never commit directly to `master`. Preserve unrelated working-tree changes and stage named paths. Push when authorized.”
- Warnings are errors in first-party code; fix their cause and add no warning opt-outs, pragma suppressions, or `-Wno-*` flags.
- Check production `Result`/`Status` values with `.has_value()`; use GoogleTest and package-owned fixtures, then regenerate and review Gazelle-managed BUILD files.

## Review Focus

1. A measurement changes from a number to an unavailable sample while a healthy peer still updates → `DesktopLoggingRun.unavailableSampleClearsOnlyItsCachedValueAndHealthyPeerContinues` (Task 1).
2. The same measurement ID exists under two protocols, or appears at repeated display positions → `DesktopLoggingRun.csvUsesCapturedProtocolIdentityAcrossRepeatedPositions` (Task 1).
3. CSV creation or writing fails while acquisition continues → `DesktopLoggingRun.csvOpenAndWriteFailuresLeaveAcquisitionActiveAndLatchReporting` and `DesktopLoggingRun.csvReenableRetriesWithCapturedSchema` (Task 1).
4. A synchronous sample-error subscriber stops run A and starts run B while A's sample batch is suspended → `DesktopLoggingRun.characterizesReplacementDuringSampleNotification` (Task 1); record any demonstrated stale-sample defect separately and leave its correction out of this extraction.
5. Connect or window destruction occurs with acquisition and CSV output active → `MainWindowTest.connectStopsAnActiveLoggingWorkerBeforeIdentification` and `MainWindowTest.windowDestructionStopsLoggingWithoutAModalNotice` (Task 2).

---

## File Map

- `src/ui/desktop/widgets/desktop_logging_run.h/.cpp` define the desktop coordinator and its copied presentation view. The coordinator borrows the engine and owns snapshot/cache/timing/CSV lifecycle.
- `src/ui/desktop/widgets/desktop_logging_run_test.cpp` tests lifecycle invariants through the coordinator with the existing scripted protocol and CSV storage seams, without constructing a window.
- `src/ui/desktop/widgets/mainwindow.h/.cpp`, `menu_actions.cpp`, and `log_operations_ssm.cpp` connect window actions and rendering to the coordinator while retaining identification sequencing and presentation.
- `src/ui/desktop/widgets/mainwindow_test.cpp` retains action, rendering, pending-selection, identification-handoff, Connect-ordering, and window-lifetime coverage; move coordinator-only lifecycle checks into the new suite.
- `src/ui/desktop/widgets/BUILD.bazel` adds the coordinator and its test target through Gazelle and wires the window target to it.
- `docs/reference/logging-contracts.md`, `docs/reference/desktop-contracts.md`, `docs/design-notes.md`, and `docs/tech-debt.md` record the enduring ownership, lifetime, and any demonstrated run-replacement defect.

## Pre-extraction characterization

Before implementing Task 1, add `MainWindowTest.characterizesReplacementDuringSampleNotification` to `mainwindow_test.cpp`. Start blocked run A with SSM `rpm`, connect a one-shot direct `MainWindow::logE` subscriber, and inject an engine `valuesUpdated` batch containing an unknown channel followed by `parameter:rpm` with numeric value `99`. The subscriber stops A through the current realtime action and starts prepared blocked run B. Assert B is active and its rpm cache becomes `"99"` after A's suspended handler resumes. Disconnect the subscriber before cleanup. This records the existing presence-check defect; migrate that exact scenario to Task 1's coordinator test. If the current implementation produces a different result, record the actual result and reconcile the characterization before extraction; never force the predicted defect.

- [ ] Add and run the baseline test: `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow --test_filter=MainWindowTest.characterizesReplacementDuringSampleNotification --test_output=errors`; expected PASS against current code.
- [ ] Commit only `src/ui/desktop/widgets/mainwindow_test.cpp` with message `test(logging): characterize replacement during sample notification`.

### Task 1: Add the desktop logging run coordinator

**Files:**
- Create: `src/ui/desktop/widgets/desktop_logging_run.h`
- Create: `src/ui/desktop/widgets/desktop_logging_run.cpp`
- Test: `src/ui/desktop/widgets/desktop_logging_run_test.cpp`
- Modify: `src/ui/desktop/widgets/BUILD.bazel`

The `qt_cc_library` target depends on `//bazel/qt:core`, `//src/platform/desktop/common/logging:logging_adapters`, `//src/platform/desktop/common/logging:logging_csv_file`, and `//src/platform/desktop/common/logging/runtime:logging_runtime`. The `fastecu_gtest` target directly depends on `:desktop_logging_run`, `//bazel/qt:core`, `//src/backend/logging/testing:scripted_logging_protocol`, `//src/backend/ports/testing:result_matchers`, `//src/platform/desktop/common/logging:logging_csv_file`, `//src/platform/desktop/common/logging/runtime:logging_runtime`, `//src/platform/desktop/common/logging/testing:failing_csv_storage`, `//src/platform/desktop/common/testing:core_application_environment`, `//src/platform/desktop/common/testing:event_helpers`, `//src/platform/desktop/common/testing:signal_recorder`, and `@googletest//:gtest`.

**Interfaces:**
- Consumes: `LoggingEngine::Start(const LogSessionConfig&, DesktopLoggingSnapshot)`, `LoggingEngine::Stop()`, `LoggingEngine::valuesUpdated`, and `LoggingEngine::sessionEnded`; `ApplyLogSample`; `LoggingCsvFile::BeginRun`, `AppendRow`, `EndRun`, and `IsOpen`.
- Produces (in `namespace fastecu::desktop::logging`):
  - `DesktopLoggingRunStart { LogSessionConfig session; DesktopLoggingSnapshot snapshot; QString csv_directory; QString csv_name_stem; }`.
  - `DesktopLoggingRunView { std::optional<DesktopLoggingSnapshot> active_snapshot; DesktopLoggerValues values; bool acquisition_running; bool csv_enabled; bool csv_open; QString csv_path; }`. `View() const` returns this value by copy. `csv_path` retains the most recently created path after close and clears when the next run starts.
  - `DesktopLoggingRun` is a `QObject` with `Q_OBJECT`, constructed as `explicit DesktopLoggingRun(LoggingEngine& engine, LoggingCsvFile csv_file = LoggingCsvFile{})`; `~DesktopLoggingRun() override` disconnects engine callbacks and performs idempotent shutdown while the borrowed engine still exists; `fastecu::Status Start(DesktopLoggingRunStart request)`; `void Stop()`; `void SetCsvEnabled(bool enabled, const QString& directory, const QString& name_stem)`; `void Shutdown()`; and `DesktopLoggingRunView View() const`.
  - Signals `presentationChanged()`, `sessionEnded(SessionEndReason, QString)`, `sampleError(QString)`, `csvError(QString)`, and `shutdownError(QString)`. Release owned run state and output before completion; a CSV failure releases only output and preserves acquisition, snapshot and cache. Publish CSV/shutdown errors only after the corresponding state transition is finished.
  - A `qt_cc_library` target `desktop_logging_run` with target visibility `//bazel/layers:ui_desktop`, plus `fastecu_gtest` target `desktop_logging_run_test`, in the existing widgets package.

- [ ] **Step 1: Add coordinator tests with exact lifecycle assertions**

Create `desktop_logging_run_test.cpp` with `TEST(DesktopLoggingRun, ...)` cases. Register one `CoreApplicationEnvironment` using `::testing::AddGlobalTestEnvironment` at executable scope, following `logging_engine_test.cpp`; constructing an environment locally does not construct a Qt application. Each ordinary test creates `LoggingEngine engine`, `DesktopLoggingRun run{engine}`, prepared snapshot, and `DesktopLoggingRunStart request`; register a `ScriptedLoggingProtocol` factory, use `SignalRecorder` for coordinator outcomes, and use `WaitUntil` for worker completion. The CSV failure tests construct `LoggingCsvFile(std::move(storage))` with `FailingCsvStorage`. Define `enum class FailurePoint { kOpen, kHeaderWrite, kRowWrite };` and run the failure cases with a parameterized fixture; keep the following assertions in the named tests:

```cpp
TEST(DesktopLoggingRun, capturedSnapshotAndFreshCacheBelongToTheRun)
{
    ASSERT_THAT(run.Start(request), fastecu::testing::IsOk());
    const auto view = run.View();
    ASSERT_TRUE(view.active_snapshot.has_value());
    EXPECT_EQ(view.active_snapshot->Target(), LoggingTarget::kTcu);
    EXPECT_EQ(view.active_snapshot->Selection(), captured_selection);
    EXPECT_TRUE(view.values.ParameterValue("SSM", "rpm").isEmpty());
}

TEST(DesktopLoggingRun, unavailableSampleClearsOnlyItsCachedValueAndHealthyPeerContinues)
{
    EXPECT_TRUE(run.View().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_EQ(run.View().values.ParameterValue("SSM", "coolant"), "90");
    EXPECT_TRUE(run.View().acquisition_running);
}

TEST(DesktopLoggingRun, csvUsesCapturedProtocolIdentityAcrossRepeatedPositions)
{
    EXPECT_THAT(csv_header, ::testing::ElementsAre("Time", "Correct CDBG"));
    EXPECT_THAT(csv_rows, ::testing::SizeIs(1));
    EXPECT_THAT(csv_rows.front().mid(1), ::testing::ElementsAre("22")); // exclude elapsed Time field
    EXPECT_THAT(csv_text, ::testing::Not(::testing::HasSubstr("Wrong SSM")));
    EXPECT_THAT(csv_text, ::testing::Not(::testing::HasSubstr("11.00")));
}

TEST(DesktopLoggingRun, csvOpenAndWriteFailuresLeaveAcquisitionActiveAndLatchReporting)
{
    EXPECT_TRUE(error_subscriber_saw_active_snapshot);
    EXPECT_FALSE(error_subscriber_saw_open_csv);
    EXPECT_TRUE(error_subscriber_saw_running_engine);
    EXPECT_EQ(csv_errors.Snapshot().size(), 1U); // one report for each injected failure
    EXPECT_TRUE(run.View().acquisition_running);
    EXPECT_EQ(run.View().values.ParameterValue("SSM", "rpm"), "43");
    EXPECT_EQ(csv_files_after_later_samples, failure == FailurePoint::kOpen ? 0 : 1);
    EXPECT_EQ(csv_errors_after_later_samples, 1U);
}

TEST(DesktopLoggingRun, csvReenableRetriesWithCapturedSchema)
{
    EXPECT_EQ(csv_errors_after_reenable, 1U);
    EXPECT_EQ(second_file_header, captured_file_header);
    EXPECT_NE(second_file_path, first_file_path);
    EXPECT_TRUE(run.View().csv_open);
}

TEST(DesktopLoggingRun, failedStartRollsBackInitializedStateAndCsv)
{
    EXPECT_TRUE(initialized_state_was_visible_to_start_diagnostic);
    EXPECT_THAT(start_result, fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_FALSE(run.View().active_snapshot.has_value());
    EXPECT_FALSE(run.View().csv_open);
    EXPECT_FALSE(engine.IsRunning());
}

TEST(DesktopLoggingRun, disablingCsvClosesOnlyOutputAndReenableUsesCapturedSchema)
{
    run.SetCsvEnabled(false, directory, stem);
    EXPECT_TRUE(run.View().acquisition_running);
    EXPECT_FALSE(run.View().csv_open);
    run.SetCsvEnabled(true, directory, next_stem);
    EXPECT_TRUE(run.View().csv_open);
    EXPECT_EQ(second_file_header, first_file_header);
    EXPECT_GT(second_file_first_elapsed, first_file_first_elapsed);
}

TEST(DesktopLoggingRun, eachNewRunCreatesAnExclusiveFileAndResetsElapsedTime)
{
    EXPECT_TRUE(run.View().csv_enabled);
    EXPECT_TRUE(run.View().csv_open);
    EXPECT_NE(second_run_path, first_run_path);
    EXPECT_EQ(second_run_header, captured_file_header);
    EXPECT_GE(second_run_elapsed, 0.0);
    EXPECT_LT(second_run_elapsed, 0.25); // seconds, direct sample injection
}

TEST(DesktopLoggingRun, newRunRetriesCsvAfterALatchedFailure)
{
    EXPECT_TRUE(run.View().csv_enabled);
    EXPECT_TRUE(run.View().csv_open);
    EXPECT_NE(second_run_path, first_run_path);
    EXPECT_EQ(second_run_header, captured_file_header);
    EXPECT_EQ(csv_errors.Snapshot().size(), 1U);
}

TEST_P(DesktopLoggingRunCompletion, engineCompletionReasonsReleaseRunAndCsv)
{
    EXPECT_FALSE(run.View().active_snapshot.has_value());
    EXPECT_FALSE(run.View().csv_open);
    EXPECT_TRUE(QFileInfo::exists(run.View().csv_path));
}

TEST(DesktopLoggingRun, completionReleasesOutputBeforeAnImmediateRestart)
{
    bool restarted = false;
    QObject::connect(&run, &DesktopLoggingRun::sessionEnded, &run,
                     [&](SessionEndReason, QString)
                     {
                         EXPECT_FALSE(run.View().active_snapshot.has_value());
                         EXPECT_FALSE(run.View().csv_open);
                         EXPECT_TRUE(run.View().values.ParameterValue("SSM", "rpm").isEmpty());
                         ASSERT_THAT(run.Start(run_b), fastecu::testing::IsOk());
                         restarted = true;
                     },
                     Qt::DirectConnection);
    run.Stop();
    EXPECT_TRUE(restarted);
    EXPECT_EQ(run.View().active_snapshot->Target(), LoggingTarget::kEcu);
}

TEST(DesktopLoggingRun, characterizesReplacementDuringSampleNotification)
{
    EXPECT_EQ(run.View().values.ParameterValue("SSM", "rpm"), "99");
    // Match the pre-extraction baseline above; do not infer the baseline
    // from the new implementation or silently add a run-identity fix.
}

TEST(DesktopLoggingRun, shutdownClosesOutputAndUsesTheNonModalOutcomePath)
{
    run.Shutdown();
    EXPECT_FALSE(engine.IsRunning());
    EXPECT_FALSE(run.View().active_snapshot.has_value());
    EXPECT_FALSE(run.View().csv_open);
    EXPECT_TRUE(run.View().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_EQ(csv_errors.Snapshot().size(), 0U);
}
```

Define `DesktopLoggingRunCompletion : ::testing::TestWithParam<EndCase>` for handshake failure (`kBadResponse` → `kHandshakeFailed`), cancellation (`Stop()` → `kStoppedByUser`), adapter loss (`kDisconnected` → `kAdapterDisconnected`), and runtime failure after reaching running (`kInternal` → `kRuntimeFailed`). Assert exactly one completion with the expected reason, stopped acquisition and released state inside its direct subscriber. A factory failure is a synchronous returned error and emits **no** engine completion: cover it with `failedStartRollsBackInitializedStateAndCsv`, parameterized for an unregistered protocol (`kInvalidConfig`), a failing factory (`kInvalidConfig`) and a throwing/null factory (`kInternal`). Assert no coordinator completion, empty cache, closed output and initialized state visible to the engine's synchronous diagnostic. Use the matching error kind instead of the single `kInternal` assertion shown above. In `csvUsesCapturedProtocolIdentityAcrossRepeatedPositions`, mutate the caller's pending selection after `Start` and verify the output remains the captured CDBG column. In the fresh-file test, use the same directory/stem in both runs, seed an existing file with sentinel bytes, stop A before starting B, and assert all paths differ and previous file contents are unchanged. Age run A beyond 500 ms before stopping it and inject B's first sample directly within 250 ms of startup; assert `Time` is below 0.25 seconds; avoid assuming a scheduled B row always arrives sooner than an A row. Use direct engine sample injection with a blocked scripted worker for timing/notification tests, and retain one actual scripted-worker sample test for first-row delivery. For re-enable, write one row before disabling, wait at least 40 ms, re-enable, and assert the new file's first elapsed field is greater than the earlier row. For header failure set `fail_writes = true` before `Start`; for row failure let `BeginRun` write its header, then set `FailingCsvStorage::fail_writes = true` before the first sample; for the open-failure case, use a nonexistent directory. After each latched failure, the re-enable test clears `fail_writes` or switches to a valid directory before retrying; `newRunRetriesCsvAfterALatchedFailure` fixes the same failure, stops run A, starts run B, and asserts a fresh file with the captured header.

Additional required assertions in Task 1 Step 1 (implement each as a named GoogleTest case):

```cpp
// secondStartPreservesTheActiveRun: start A, call Start(B) without Stop.
EXPECT_THAT(second_start, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
EXPECT_EQ(after.active_snapshot->Target(), before.active_snapshot->Target());
EXPECT_EQ(after.csv_path, before.csv_path);
EXPECT_TRUE(after.acquisition_running);

// sampleErrorSubscriberStopsBeforeHealthyPeer: unknown channel then rpm=99;
// a one-shot direct sampleError subscriber calls run.Stop().
EXPECT_EQ(sample_errors.Count(), 1);
EXPECT_FALSE(run.View().active_snapshot.has_value());
EXPECT_TRUE(run.View().values.ParameterValue("SSM", "rpm").isEmpty());
EXPECT_EQ(rows_after_stop, rows_before_batch);

// csvErrorSubscriberStopsAfterFailureStateCommits: injected row failure;
// subscriber observes closed CSV and active acquisition, then calls Stop.
EXPECT_FALSE(run.View().acquisition_running);
EXPECT_FALSE(run.View().active_snapshot.has_value());
EXPECT_EQ(csv_errors.Count(), 1);

// copiedViewSurvivesStopAndRestart: retain View(A), stop/start B and sample B.
EXPECT_EQ(saved_a.active_snapshot->Target(), LoggingTarget::kTcu);
EXPECT_EQ(saved_a.values.ParameterValue("SSM", "rpm"), "42");
EXPECT_EQ(run.View().values.ParameterValue("SSM", "rpm"), "43");

// shutdownIsIdempotentAndCannotRestart: completion subscriber attempts restart
// if called; ordinary completion must not be published during shutdown.
run.Shutdown();
run.Shutdown();
EXPECT_EQ(completions.Count(), 0);
EXPECT_THAT(run.Start(request), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
EXPECT_FALSE(engine.IsRunning());
```

For unavailable samples, seed `rpm=42` first, then inject unavailable rpm and healthy coolant `90` in one batch; assert empty rpm in the cache **and the CSV field**, coolant `90`, one retained row and no acquisition stop. Assert `presentationChanged` observers see the fully applied batch. For retry tests, verify an ordinary repeated `SetCsvEnabled(true, ...)` does not retry a latched failure; explicitly disable then enable to retry. Disconnect the immediate-restart completion subscriber after its first invocation to prevent teardown from restarting recursively.

- [ ] **Step 2: Run the new test target to verify the missing seam**

Create the header declarations, minimal linkable method stubs and Gazelle-generated target first (`python3 scripts/gazelle_check.py --fix`); do not count a nonexistent Bazel label as the failing test. Run: `bazel test --config=release //src/ui/desktop/widgets:desktop_logging_run_test --test_output=errors`.

Expected: the new target builds and named lifecycle assertions fail against the stubs; fix harness/compilation problems before implementing behavior.

- [ ] **Step 3: Implement `DesktopLoggingRun` and its public value types**

Confine all coordinator calls and engine connections to the owning Qt thread. `Start` rejects an already active run with `kInvalidConfig` before modifying any snapshot, cache, timer or file; callers stop explicitly before starting a replacement. Initialize the copied active snapshot, cache via `DesktopLoggerValues::BeginRun`, elapsed timer, CSV path, and failure latch before calling `LoggingEngine::Start`. Do not emit a coordinator notification with half-initialized state. Buffer a CSV creation error until acquisition startup has returned, so a file failure does not block acquisition; on a returned startup error, release CSV, snapshot, and cache before publishing buffered outcomes. Keep CSV enablement across runs; reset the failure latch on a new run and on a false-to-true CSV transition only. Connect engine samples and completion to the coordinator, preserve the per-sample active-run check, continue healthy peers after sample errors, and close CSV, clear run state, then publish completion. `Stop()` must not mutate state after `LoggingEngine::Stop()` returns, because its synchronous completion subscriber may already have started the next run. In `Shutdown`, set terminal shutdown mode before stopping; reject subsequent `Start` with `kInvalidConfig`, suppress ordinary completion/presentation/CSV notices during shutdown, disconnect engine callbacks before destruction, and report a close failure through `shutdownError`. Repeat `Shutdown` safely. `FailingCsvStorage` only injects write errors; `QFile::flush` is not a virtual seam for a deterministic close-only failure. Preserve the checked `EndRun` error routing without claiming that toggling `fail_writes` proves a close error. Use no new generic storage interface.

For normal completion, collect any `EndRun` error, release snapshot/cache/timer and capture completion arguments before the first notification. Emit completion before any collected close-error notice; do not mutate state after a subscriber can restart. For samples, preserve the existing per-sample presence check and error-first notification ordering, recheck presence after the batch before rendering/writing, and leave replacement-run characterization separate. CSV errors must never clear acquisition state. `presentationChanged` carries no borrowed state and is emitted only after committed transitions; the window obtains a fresh view.

- [ ] **Step 4: Generate the Bazel targets**

Run: `python3 scripts/gazelle_check.py --fix`

Expected: Gazelle emits the coordinator library and GoogleTest target in the widgets package with no visibility widening.

- [ ] **Step 5: Run the coordinator suite**

Run: `bazel test --config=release //src/ui/desktop/widgets:desktop_logging_run_test --test_output=errors`

Expected: PASS; review Gazelle's target and dependency output, keeping the library within the existing widgets package and using the emitted target names if Gazelle adjusts them.

- [ ] **Step 6: Commit the coordinator**

```sh
git add src/ui/desktop/widgets/desktop_logging_run.h src/ui/desktop/widgets/desktop_logging_run.cpp src/ui/desktop/widgets/desktop_logging_run_test.cpp src/ui/desktop/widgets/BUILD.bazel
git commit -m "refactor(logging): add desktop run coordinator"
```

### Task 2: Route window logging through the coordinator

**Files:**
- Modify: `src/ui/desktop/widgets/mainwindow.h`
- Modify: `src/ui/desktop/widgets/mainwindow.cpp`
- Modify: `src/ui/desktop/widgets/menu_actions.cpp`
- Modify: `src/ui/desktop/widgets/log_operations_ssm.cpp`
- Modify: `src/ui/desktop/widgets/mainwindow_test.cpp`
- Modify: `src/ui/desktop/widgets/BUILD.bazel` (if Gazelle reports a dependency change)

**Interfaces:**
- Consumes: `DesktopLoggingRunStart`, `DesktopLoggingRun::Start`, `Stop`, `SetCsvEnabled`, `Shutdown`, `View`, and the five signals defined in Task 1.
- Produces: a `MainWindow`-owned `std::unique_ptr<DesktopLoggingRun> logging_run_`. No window handler calls `LoggingEngine::Start` or `Stop`, and no borrowed snapshot/cache reference survives a callback.

- [ ] **Step 1: Rewrite the Connect ordering test and add the destruction case**

In `connectStopsAnActiveLoggingWorkerBeforeIdentification`, start through the window and assert that the run view has an active snapshot and open CSV before Connect; in the identification handoff assert `services.logging_engine.IsRunning()` is false, `View().active_snapshot` is empty, and `View().csv_open` is false. Add `windowDestructionStopsLoggingWithoutAModalNotice`: start a blocked scripted run with its CSV file open, destroy the window, then assert the engine is stopped and no modal notice was shown. Task 1's shutdown test asserts output closure through the coordinator view.

- [ ] **Step 2: Run the affected window suite to verify the new integration expectation**

Run: `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow --test_output=errors`

Expected: failure showing the missing coordinator ownership/integration. Establish the declared `logging_run_` seam so the test builds before using a failing behavioral assertion as the integration red step.

- [ ] **Step 3: Replace window-owned lifecycle fields and route each entry point**

Construct `logging_run_` with `services_.logging_engine`; move the snapshot, `DesktopLoggerValues`, elapsed timer, CSV file, failure latch, and last CSV path out of `MainWindow`. After identification succeeds, build `DesktopLoggingRunStart` from the existing selected protocol, policy, target, prepared snapshot, effective datalog directory, and timestamp stem. Route realtime stop, Connect, file logging toggles, and destruction through `Stop`, `SetCsvEnabled`, and `Shutdown`. Keep the pending connection continuation and startup dialogs in the window. Keep `reportLoggingCsvError` as the presentation handler, but remove its duplicate failure latch; make `restoreLoggingUiState` update controls and idle widgets without cleaning coordinator state.

Render from one `View()` copy per operation. Change the declaration and definition to `fastecu::logging::LoggerSelection displayedLoggingSelection() const`, derive active protocol/metadata from `active_snapshot`, and refresh widgets on `presentationChanged`. Explicitly migrate `updateLoggingPendingState()`, `updateLogboxes(const QString&)`, and `updateLogboxValues(const QString&)` in `mainwindow.cpp`; their metadata pointers must point into the local view copy. Remove the startup `logger_values_.Initialize` and `log_file_timer_` allocation, plus obsolete `beginLoggingCsv`, `endLoggingCsv`, `logToFile`, and `handleLoggingValuesUpdated` declarations/definitions. Idle rendering uses the pending model and unavailable readings. Retain `logging_state_` only for the existing pending identification/UI request; derive acquisition facts and CSV enablement from `View()` rather than keeping `write_datalog_to_file_` as another owner. `updateLogboxValues` currently pumps events via `delay(1)`, between sample application and CSV writing. Characterize a queued stop delivered during that delay before changing this ordering. Keep the rendering path free of event pumping when driven by the coordinator, so a batch/row transition finishes before `presentationChanged`; if the baseline demonstrates a user-visible difference, report it and obtain a separate correction decision before adopting that ordering. Route `sampleError` to `logE`, `csvError` to the existing CSV log/modal handler, `shutdownError` to logging only, and `sessionEnded` to the existing completion presentation after the coordinator has released its state.

Move `csvNewRunsOwnNewFiles`, `csvCreationFailureLeavesDisplayActive`, `csvWriteFailureLeavesDisplayActive`, `csvSessionEndsCloseTheirFile`, and `csvSharedIdProtocolIdentity` from `MainWindowTest` into `DesktopLoggingRunTest`; use `DesktopLoggingRunCompletion.engineCompletionReasonsReleaseRunAndCsv` for the former completion-reason cases. Keep `loggingDisplayErrorContinuesOtherSamples` as a focused window check: emit `valuesUpdated` with an unknown channel followed by `parameter:rpm=42`, assert one `logE`, assert the run view contains `42`, and assert acquisition is still active. Move the existing `csvEscapesNamesAndValues` composition case to the coordinator suite as well; retain the serializer/file unit tests that own escaping policy. Do not discard the composition assertion that escaped captured names and first-row values reach the output. Keep tests for actions, rendered OFF/ON and literal text, pending selection indication, protocol/target selection, invalid-preparation dialogs, identification handoff, Connect ordering, and window destruction. Read lifecycle state through `View()` in the remaining window tests. Keep `windowDestructionJoinsIdentificationWithoutContinuingLogging`; call `connection_coordinator_->shutdown()` before `logging_run_->Shutdown()` in the destructor. Keep engine diagnostic routing owned by composition; the coordinator consumes values/completion and emits sample/CSV errors without forwarding the same engine diagnostic twice.

- [ ] **Step 4: Regenerate window dependencies**

Run: `python3 scripts/gazelle_check.py --fix`; expected only intended target/dependency changes and no visibility widening.

- [ ] **Step 5: Run the coordinator and window suites**

Run: `bazel test --config=release //src/ui/desktop/widgets:desktop_logging_run_test //src/ui/desktop/widgets:test_mainwindow --test_output=errors`

Expected: PASS; the window tests still cover presentation and connection sequencing, while lifecycle tests run without constructing widgets.

- [ ] **Step 6: Commit the window integration**

```sh
git add src/ui/desktop/widgets/mainwindow.h src/ui/desktop/widgets/mainwindow.cpp src/ui/desktop/widgets/menu_actions.cpp src/ui/desktop/widgets/log_operations_ssm.cpp src/ui/desktop/widgets/mainwindow_test.cpp src/ui/desktop/widgets/BUILD.bazel
git commit -m "refactor(logging): route window lifecycle through run owner"
```

### Task 3: Record the new desktop ownership contract

**Files:**
- Modify: `docs/reference/logging-contracts.md`
- Modify: `docs/reference/desktop-contracts.md`
- Modify: `docs/design-notes.md`
- Modify: `docs/tech-debt.md` (record the characterized replacement-run defect separately)
- Include in delivered history: `docs/superpowers/specs/2026-10-13-desktop-logging-run-ownership.md` and this plan.

**Interfaces:**
- Consumes: the implemented `DesktopLoggingRun` ownership, copied-view, notification, cleanup, and window-lifetime behavior from Tasks 1–2.
- Produces: current documentation that identifies the logging reference as the owner of the contract and links desktop composition lifetime notes to it.

- [ ] **Step 1: Update the owning contract and its design links**

In `logging-contracts.md`, describe that the window-owned desktop run coordinator borrows the composition-owned engine and owns the active snapshot, display cache, elapsed timing, CSV lifecycle, and failure latch; document by-value presentation views and release-before-notification ordering. In `desktop-contracts.md`, record that Connect, normal stop, and window shutdown synchronously release the run before facade reuse or destruction, with a human-readable link to the logging contract. Update the Logging section of `design-notes.md` to locate the desktop run/cache/CSV responsibilities and link to the owning contract. Add a separate item under `### P1: Resolve known correctness gaps` in `docs/tech-debt.md` describing the stale batch reaching a replacement run if the characterization test observes it; do not include a correction in this plan.

- [ ] **Step 2: Check documentation links and whitespace**

Run: `prek run lychee --all-files`

Expected: PASS with all cross-document links using human-readable Markdown text.

- [ ] **Step 3: Check the scoped diff**

Run: `git diff --check`

Expected: PASS with no whitespace errors.

- [ ] **Step 4: Commit the durable contract updates**

```sh
git add docs/reference/logging-contracts.md docs/reference/desktop-contracts.md docs/design-notes.md docs/tech-debt.md docs/superpowers/specs/2026-10-13-desktop-logging-run-ownership.md docs/superpowers/plans/2026-10-13-desktop-logging-run-ownership.md
git commit -m "docs(logging): record desktop run ownership"
```

Keep the active spec and this plan in the checkout through implementation delivery. Their original contents are currently untracked, so include both named paths in the delivered feature change before any later cleanup removes them; do not introduce and delete them in the same squash change. Leave other untracked `docs/superpowers/` files unstaged.

## Plan review notes

The reviewed plan distinguishes returned startup failures from worker completion,
registers the Qt application once per test executable, covers header and row
write failures separately, and measures replacement reentrancy before extraction.
The implementation must preserve existing engine generation-fencing tests; those
cover queued worker events, not an already-entered desktop batch. Close-only error
injection is a known test-seam limitation, not evidence that close handling passed.
Rendering's event-pumping order requires the explicit baseline check in Task 2.

## Final delivery gates

After all three tasks, regenerate and review managed targets, then run the repository gates:

- [ ] Run `python3 scripts/gazelle_check.py --fix` and review all BUILD changes.
- [ ] Run `prek run --all-files` and keep required formatting changes.
- [ ] Run `bazel run //:clang_tidy_report_changed`.
- [ ] Run `bazel test --config=release //...`.
- [ ] Run `bazel build --config=release //:fastecu`.
