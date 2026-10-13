#include "src/ui/desktop/widgets/desktop_logging_run.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>
#include <stdexcept>

#include "src/backend/logging/testing/scripted_logging_protocol.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/logging/testing/failing_csv_storage.h"
#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include "src/platform/desktop/common/testing/signal_recorder.h"

namespace fastecu::desktop::logging
{
namespace
{
using namespace std::chrono_literals;
using namespace fastecu::logging;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::SignalRecorder;
using fastecu::testing::WaitUntil;

DesktopLoggingRunStart makeRequest(const QString& directory, LoggingTarget target = LoggingTarget::kTcu,
                                   const std::string& protocol = "SSM", const std::string& name = "rpm")
{
    LoggerModel model;
    LoggerDefinition definition{.parameters = {{.protocol = protocol,
                                                .id = "rpm",
                                                .name = name,
                                                .address = "10",
                                                .length = "1",
                                                .enabled = true,
                                                .conversions = {{.units = "", .expr = "x", .format = "0"}}},
                                               {.protocol = protocol,
                                                .id = "coolant",
                                                .name = "coolant",
                                                .address = "20",
                                                .length = "1",
                                                .enabled = true,
                                                .conversions = {{.units = "", .expr = "x", .format = "0"}}}}};
    if (protocol == "CDBG")
    {
        definition.parameters.push_back({.protocol = "SSM",
                                         .id = "rpm",
                                         .name = "Wrong SSM",
                                         .address = "10",
                                         .length = "1",
                                         .enabled = true,
                                         .conversions = {{.units = "", .expr = "x", .format = "0.00"}}});
    }
    EXPECT_TRUE(model.InstallDefinition(std::move(definition)));
    model.SetSelection({.protocol = protocol, .gauge_ids = {"rpm"}, .lower_panel_ids = {"rpm", "coolant"}});
    auto snapshot =
        MakeDesktopLoggingSnapshot(model, protocol == "CDBG" ? LoggingProtocolId::kCdbg : LoggingProtocolId::kSsm,
                                   QString::fromStdString(protocol),
                                   {.poll_timeout = 50ms,
                                    .car_silence_miss_threshold = 100000,
                                    .reconnect_attempt_threshold = 100001,
                                    .reconnect_retry_period = 0},
                                   target);
    EXPECT_THAT(snapshot, IsOk());
    return {{QString::fromStdString(protocol)}, *snapshot, directory, "run"};
}
QByteArray readFile(const QString& path)
{
    QFile file{path};
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void registerBlocked(LoggingEngine& engine, const QString& protocol = "SSM")
{
    engine.RegisterProtocol(protocol,
                            [](const auto&)
                            {
                                auto source = std::make_unique<ScriptedLoggingProtocol>();
                                source->BlockPollUntilCancelled();
                                return source;
                            });
}
class DesktopLoggingRunTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory_.isValid());
        registerBlocked(engine_);
    }
    QTemporaryDir directory_;
    LoggingEngine engine_;
    DesktopLoggingRun run_{engine_};
};

TEST_F(DesktopLoggingRunTest, capturedSnapshotAndFreshCacheBelongToTheRun)
{
    auto request = makeRequest(directory_.path());
    ASSERT_THAT(run_.start(request), IsOk());
    request = makeRequest(directory_.path(), LoggingTarget::kEcu);
    const auto view = run_.view();
    ASSERT_TRUE(view.active_snapshot.has_value());
    EXPECT_EQ(view.active_snapshot->Target(), LoggingTarget::kTcu);
    EXPECT_THAT(view.active_snapshot->Selection().lower_panel_ids, ::testing::ElementsAre("rpm", "coolant"));
    EXPECT_TRUE(view.values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_TRUE(view.acquisition_running);
}
TEST_F(DesktopLoggingRunTest, unavailableSampleClearsOnlyItsCachedValueAndHealthyPeerContinues)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = std::nullopt},
                                {.channel_id = "parameter:coolant", .numeric_value = 90}});
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_EQ(run_.view().values.ParameterValue("SSM", "coolant"), "90");
    EXPECT_TRUE(run_.view().acquisition_running);
    EXPECT_TRUE(readFile(run_.view().csv_path).contains(",,90\n"));
}
TEST_F(DesktopLoggingRunTest, presentationObserversSeeTheCompleteBatch)
{
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    bool notified = false;
    QObject::connect(&run_, &DesktopLoggingRun::presentationChanged, &run_,
                     [&]
                     {
                         const auto view = run_.view();
                         EXPECT_EQ(view.values.ParameterValue("SSM", "rpm"), "42");
                         EXPECT_EQ(view.values.ParameterValue("SSM", "coolant"), "90");
                         notified = true;
                     });
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42},
                                {.channel_id = "parameter:coolant", .numeric_value = 90}});
    EXPECT_TRUE(notified);
}
TEST_F(DesktopLoggingRunTest, failedAcquisitionReportsBufferedCsvFailureAfterRollback)
{
    engine_.RegisterProtocol("SSM", [](const auto&) -> Result<std::unique_ptr<LoggingProtocol>>
                             { return Fail(ErrorKind::kInvalidConfig, "factory failure"); });
    const auto missing = directory_.path() + "/absent";
    run_.setCsvEnabled(true, missing, "run");
    SignalRecorder errors{&run_, &DesktopLoggingRun::csvError};
    QObject::connect(&run_, &DesktopLoggingRun::csvError, &run_,
                     [&](const QString&)
                     {
                         EXPECT_FALSE(run_.view().active_snapshot.has_value());
                         EXPECT_FALSE(run_.view().csv_open);
                         EXPECT_FALSE(run_.view().acquisition_running);
                     });
    EXPECT_THAT(run_.start(makeRequest(missing)), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(errors.Count(), 1U);
}

TEST_F(DesktopLoggingRunTest, secondStartPreservesTheActiveRun)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    const auto before = run_.view();
    EXPECT_THAT(run_.start(makeRequest(directory_.path(), LoggingTarget::kEcu)), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(run_.view().active_snapshot->Target(), before.active_snapshot->Target());
    EXPECT_EQ(run_.view().csv_path, before.csv_path);
    EXPECT_TRUE(run_.view().acquisition_running);
}
TEST_F(DesktopLoggingRunTest, sampleErrorSubscriberStopsBeforeHealthyPeer)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    const auto path = run_.view().csv_path;
    SignalRecorder errors{&run_, &DesktopLoggingRun::sampleError};
    QObject::connect(&run_, &DesktopLoggingRun::sampleError, &run_, [&](const QString&) { run_.stop(); });
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_EQ(errors.Count(), 1U);
    EXPECT_FALSE(run_.view().active_snapshot.has_value());
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_EQ(readFile(path), "Time,rpm,coolant\n");
}
TEST_F(DesktopLoggingRunTest, replacementDuringSampleErrorDiscardsOldBatch)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    const auto oldPath = run_.view().csv_path;
    bool replaced = false;
    QObject::connect(&run_, &DesktopLoggingRun::sampleError, &run_,
                     [&](const QString&)
                     {
                         if (replaced)
                         {
                             return;
                         }
                         replaced = true;
                         run_.stop();
                         EXPECT_FALSE(run_.view().csv_open);
                         ASSERT_THAT(run_.start(makeRequest(directory_.path(), LoggingTarget::kEcu)), IsOk());
                     });
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_TRUE(replaced);
    ASSERT_TRUE(run_.view().active_snapshot.has_value());
    EXPECT_EQ(run_.view().active_snapshot->Target(), LoggingTarget::kEcu);
    EXPECT_TRUE(run_.view().acquisition_running);
    EXPECT_TRUE(run_.view().csv_open);
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    const auto newPath = run_.view().csv_path;
    EXPECT_NE(newPath, oldPath);
    EXPECT_EQ(readFile(newPath), "Time,rpm,coolant\n");
    EXPECT_EQ(readFile(oldPath), "Time,rpm,coolant\n");
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 43}});
    EXPECT_EQ(run_.view().values.ParameterValue("SSM", "rpm"), "43");
    EXPECT_TRUE(readFile(newPath).contains(",43,\n"));
    EXPECT_FALSE(readFile(newPath).contains(",99,"));
    EXPECT_EQ(readFile(oldPath), "Time,rpm,coolant\n");
}
TEST_F(DesktopLoggingRunTest, replacementDuringPresentationDiscardsOldCsvRow)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    const auto oldPath = run_.view().csv_path;
    bool replaced = false;
    QObject::connect(&run_, &DesktopLoggingRun::presentationChanged, &run_,
                     [&]
                     {
                         if (replaced)
                         {
                             return;
                         }
                         replaced = true;
                         EXPECT_EQ(run_.view().values.ParameterValue("SSM", "rpm"), "99");
                         run_.stop();
                         EXPECT_FALSE(run_.view().csv_open);
                         // Identical selections must still identify a distinct run.
                         ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
                     });
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_TRUE(replaced);
    ASSERT_TRUE(run_.view().active_snapshot.has_value());
    EXPECT_TRUE(run_.view().acquisition_running);
    EXPECT_TRUE(run_.view().csv_open);
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    const auto newPath = run_.view().csv_path;
    EXPECT_NE(newPath, oldPath);
    EXPECT_EQ(readFile(newPath), "Time,rpm,coolant\n");
    EXPECT_EQ(readFile(oldPath), "Time,rpm,coolant\n");
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 43}});
    EXPECT_EQ(run_.view().values.ParameterValue("SSM", "rpm"), "43");
    EXPECT_TRUE(readFile(newPath).contains(",43,\n"));
    EXPECT_FALSE(readFile(newPath).contains(",99,"));
    EXPECT_EQ(readFile(oldPath), "Time,rpm,coolant\n");
}
TEST_F(DesktopLoggingRunTest, shutdownDuringSampleErrorDiscardsOldBatch)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    const auto path = run_.view().csv_path;
    SignalRecorder presentations{&run_, &DesktopLoggingRun::presentationChanged};
    SignalRecorder errors{&run_, &DesktopLoggingRun::sampleError};
    QObject::connect(&run_, &DesktopLoggingRun::sampleError, &run_, [&](const QString&) { run_.shutdown(); });
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 99}});
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 43}});
    EXPECT_EQ(errors.Count(), 1U);
    EXPECT_EQ(presentations.Count(), 0U);
    EXPECT_FALSE(run_.view().active_snapshot.has_value());
    EXPECT_FALSE(run_.view().acquisition_running);
    EXPECT_FALSE(run_.view().csv_open);
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_EQ(readFile(path), "Time,rpm,coolant\n");
}
TEST_F(DesktopLoggingRunTest, failedReplacementDoesNotResumeOldBatch)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    const auto oldPath = run_.view().csv_path;
    SignalRecorder presentations{&run_, &DesktopLoggingRun::presentationChanged};
    bool attempted = false;
    QObject::connect(&run_, &DesktopLoggingRun::sampleError, &run_,
                     [&](const QString&)
                     {
                         if (attempted)
                         {
                             return;
                         }
                         attempted = true;
                         run_.stop();
                         engine_.RegisterProtocol("SSM", [](const auto&) -> Result<std::unique_ptr<LoggingProtocol>>
                                                  { return Fail(ErrorKind::kInvalidConfig, "factory failure"); });
                         EXPECT_THAT(run_.start(makeRequest(directory_.path())), IsErr(ErrorKind::kInvalidConfig));
                     });
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_TRUE(attempted);
    EXPECT_EQ(presentations.Count(), 0U);
    EXPECT_FALSE(run_.view().active_snapshot.has_value());
    EXPECT_FALSE(run_.view().acquisition_running);
    EXPECT_FALSE(run_.view().csv_open);
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_NE(run_.view().csv_path, oldPath);
    EXPECT_EQ(readFile(run_.view().csv_path), "Time,rpm,coolant\n");
    EXPECT_EQ(readFile(oldPath), "Time,rpm,coolant\n");
}
TEST_F(DesktopLoggingRunTest, sampleErrorKeepsHealthyPeerWhenRunIsUnchanged)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    SignalRecorder errors{&run_, &DesktopLoggingRun::sampleError};
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 43}});
    EXPECT_EQ(errors.Count(), 1U);
    EXPECT_TRUE(run_.view().active_snapshot.has_value());
    EXPECT_TRUE(run_.view().acquisition_running);
    EXPECT_EQ(run_.view().values.ParameterValue("SSM", "rpm"), "43");
    EXPECT_TRUE(readFile(run_.view().csv_path).contains(",43,\n"));
}
TEST_F(DesktopLoggingRunTest, stopDuringPresentationDiscardsPendingCsvRow)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    const auto path = run_.view().csv_path;
    bool stopped = false;
    QObject::connect(&run_, &DesktopLoggingRun::presentationChanged, &run_,
                     [&]
                     {
                         stopped = true;
                         EXPECT_EQ(run_.view().values.ParameterValue("SSM", "rpm"), "99");
                         run_.stop();
                     });
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_TRUE(stopped);
    EXPECT_FALSE(run_.view().active_snapshot.has_value());
    EXPECT_FALSE(run_.view().csv_open);
    EXPECT_EQ(readFile(path), "Time,rpm,coolant\n");
}
TEST_F(DesktopLoggingRunTest, copiedViewSurvivesStopAndRestart)
{
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    const auto saved = run_.view();
    run_.stop();
    ASSERT_THAT(run_.start(makeRequest(directory_.path(), LoggingTarget::kEcu)), IsOk());
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 43}});
    EXPECT_EQ(saved.active_snapshot->Target(), LoggingTarget::kTcu);
    EXPECT_EQ(saved.values.ParameterValue("SSM", "rpm"), "42");
    EXPECT_EQ(run_.view().values.ParameterValue("SSM", "rpm"), "43");
}
TEST_F(DesktopLoggingRunTest, completionReleasesOutputBeforeAnImmediateRestart)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    bool restarted = false;
    const auto subscription =
        QObject::connect(&run_, &DesktopLoggingRun::sessionEnded, &run_,
                         [&](SessionEndReason, const QString&)
                         {
                             if (restarted)
                             {
                                 return;
                             }
                             restarted = true;
                             EXPECT_FALSE(run_.view().active_snapshot.has_value());
                             EXPECT_FALSE(run_.view().csv_open);
                             EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
                             ASSERT_THAT(run_.start(makeRequest(directory_.path(), LoggingTarget::kEcu)), IsOk());
                         });
    run_.stop();
    QObject::disconnect(subscription);
    EXPECT_TRUE(restarted);
    ASSERT_TRUE(run_.view().active_snapshot.has_value());
    EXPECT_EQ(run_.view().active_snapshot->Target(), LoggingTarget::kEcu);
    EXPECT_TRUE(run_.view().csv_open);
}
TEST_F(DesktopLoggingRunTest, shutdownClosesOutputAndUsesTheNonModalOutcomePath)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    SignalRecorder errors{&run_, &DesktopLoggingRun::csvError};
    SignalRecorder completions{&run_, &DesktopLoggingRun::sessionEnded};
    run_.shutdown();
    run_.shutdown();
    EXPECT_FALSE(engine_.IsRunning());
    EXPECT_FALSE(run_.view().active_snapshot.has_value());
    EXPECT_FALSE(run_.view().csv_open);
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_EQ(errors.Count(), 0U);
    EXPECT_EQ(completions.Count(), 0U);
    EXPECT_THAT(run_.start(makeRequest(directory_.path())), IsErr(ErrorKind::kInvalidConfig));
}
TEST_F(DesktopLoggingRunTest, disablingCsvClosesOnlyOutputAndReenableUsesCapturedSchema)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    const auto first = run_.view().csv_path;
    const auto firstRow = readFile(first).split('\n').at(1).split(',').at(0).toDouble();
    run_.setCsvEnabled(false, directory_.path(), "run");
    EXPECT_TRUE(run_.view().acquisition_running);
    EXPECT_FALSE(run_.view().csv_open);
    QThread::msleep(40);
    run_.setCsvEnabled(true, directory_.path(), "run");
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 43}});
    const auto second = run_.view().csv_path;
    EXPECT_NE(second, first);
    EXPECT_EQ(readFile(second).split('\n').front(), readFile(first).split('\n').front());
    EXPECT_GT(readFile(second).split('\n').at(1).split(',').at(0).toDouble(), firstRow);
}
TEST_F(DesktopLoggingRunTest, eachNewRunCreatesAnExclusiveFileAndResetsElapsedTime)
{
    const auto existing = QDir(directory_.path()).filePath("run.csv");
    QFile sentinel{existing};
    ASSERT_TRUE(sentinel.open(QIODevice::WriteOnly));
    ASSERT_EQ(sentinel.write("preserve"), 8);
    sentinel.close();
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    QThread::msleep(500);
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    const auto first = run_.view().csv_path;
    const auto bytes = readFile(first);
    run_.stop();
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    emit engine_.valuesUpdated({});
    const auto second = run_.view().csv_path;
    EXPECT_TRUE(run_.view().csv_enabled);
    EXPECT_NE(first, second);
    EXPECT_NE(first, existing);
    EXPECT_EQ(readFile(existing), "preserve");
    EXPECT_EQ(readFile(first), bytes);
    EXPECT_EQ(readFile(second).split('\n').front(), bytes.split('\n').front());
    EXPECT_LT(readFile(second).split('\n').at(1).split(',').at(0).toDouble(), 0.25);
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
}

enum class FailurePoint
{
    kOpen,
    kHeaderWrite,
    kRowWrite
};
class DesktopLoggingRunFailure : public ::testing::TestWithParam<FailurePoint>
{
};
TEST_P(DesktopLoggingRunFailure, csvOpenAndWriteFailuresLeaveAcquisitionActiveAndLatchReporting)
{
    QTemporaryDir directory;
    LoggingEngine engine;
    registerBlocked(engine);
    auto storage = std::make_unique<testing::FailingCsvStorage>();
    auto *injected = storage.get();
    injected->fail_writes = GetParam() == FailurePoint::kHeaderWrite;
    DesktopLoggingRun run{engine, LoggingCsvFile{std::move(storage)}};
    const auto destination = GetParam() == FailurePoint::kOpen ? directory.path() + "/absent" : directory.path();
    run.setCsvEnabled(true, destination, "run");
    SignalRecorder errors{&run, &DesktopLoggingRun::csvError};
    QObject::connect(&run, &DesktopLoggingRun::csvError, &run,
                     [&](const QString&)
                     {
                         EXPECT_TRUE(run.view().active_snapshot.has_value());
                         EXPECT_TRUE(run.view().acquisition_running);
                         EXPECT_FALSE(run.view().csv_open);
                     });
    ASSERT_THAT(run.start(makeRequest(destination)), IsOk());
    injected->fail_writes = GetParam() != FailurePoint::kOpen;
    emit engine.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    emit engine.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 43}});
    run.setCsvEnabled(true, directory.path(), "run");
    EXPECT_EQ(errors.Count(), 1U);
    EXPECT_EQ(run.view().values.ParameterValue("SSM", "rpm"), "43");
    EXPECT_TRUE(run.view().acquisition_running);
    EXPECT_FALSE(run.view().csv_open);
    EXPECT_EQ(QDir(directory.path()).entryList({"*.csv"}, QDir::Files).size(),
              GetParam() == FailurePoint::kOpen ? 0 : 1);
    injected->fail_writes = false;
    run.setCsvEnabled(false, directory.path(), "run");
    run.setCsvEnabled(true, directory.path(), "run");
    EXPECT_TRUE(run.view().csv_open);
    EXPECT_TRUE(readFile(run.view().csv_path).startsWith("Time,rpm,coolant\n"));
    EXPECT_EQ(errors.Count(), 1U);
}
TEST_P(DesktopLoggingRunFailure, newRunRetriesCsvAfterALatchedFailure)
{
    QTemporaryDir directory;
    LoggingEngine engine;
    registerBlocked(engine);
    auto storage = std::make_unique<testing::FailingCsvStorage>();
    auto *injected = storage.get();
    injected->fail_writes = GetParam() == FailurePoint::kHeaderWrite;
    DesktopLoggingRun run{engine, LoggingCsvFile{std::move(storage)}};
    const auto destination = GetParam() == FailurePoint::kOpen ? directory.path() + "/absent" : directory.path();
    run.setCsvEnabled(true, destination, "run");
    SignalRecorder errors{&run, &DesktopLoggingRun::csvError};
    ASSERT_THAT(run.start(makeRequest(destination)), IsOk());
    injected->fail_writes = GetParam() != FailurePoint::kOpen;
    emit engine.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    const auto first = run.view().csv_path;
    run.stop();
    injected->fail_writes = false;
    ASSERT_THAT(run.start(makeRequest(directory.path())), IsOk());
    EXPECT_TRUE(run.view().csv_enabled);
    EXPECT_TRUE(run.view().csv_open);
    EXPECT_NE(first, run.view().csv_path);
    EXPECT_TRUE(readFile(run.view().csv_path).startsWith("Time,rpm,coolant\n"));
    EXPECT_EQ(errors.Count(), 1U);
}
INSTANTIATE_TEST_SUITE_P(Failures, DesktopLoggingRunFailure,
                         ::testing::Values(FailurePoint::kOpen, FailurePoint::kHeaderWrite, FailurePoint::kRowWrite));

TEST_F(DesktopLoggingRunTest, csvErrorSubscriberStopsAfterFailureStateCommits)
{
    auto storage = std::make_unique<testing::FailingCsvStorage>();
    auto *injected = storage.get();
    DesktopLoggingRun run{engine_, LoggingCsvFile{std::move(storage)}};
    run.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run.start(makeRequest(directory_.path())), IsOk());
    SignalRecorder errors{&run, &DesktopLoggingRun::csvError};
    QObject::connect(&run, &DesktopLoggingRun::csvError, &run,
                     [&](const QString&)
                     {
                         EXPECT_TRUE(run.view().acquisition_running);
                         EXPECT_FALSE(run.view().csv_open);
                         run.stop();
                     });
    injected->fail_writes = true;
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    EXPECT_FALSE(run.view().acquisition_running);
    EXPECT_FALSE(run.view().active_snapshot.has_value());
    EXPECT_EQ(errors.Count(), 1U);
}

enum class StartFailure
{
    kMissingRegistration,
    kFactory,
    kThrow,
    kNull
};
class DesktopLoggingRunStartFailure : public ::testing::TestWithParam<StartFailure>
{
};
TEST_P(DesktopLoggingRunStartFailure, failedStartRollsBackInitializedStateAndCsv)
{
    QTemporaryDir directory;
    LoggingEngine engine;
    const auto failure = GetParam();
    if (failure != StartFailure::kMissingRegistration)
    {
        engine.RegisterProtocol("SSM",
                                [failure](const auto&) -> Result<std::unique_ptr<LoggingProtocol>>
                                {
                                    if (failure == StartFailure::kThrow)
                                    {
                                        throw std::runtime_error("factory failure");
                                    }
                                    if (failure == StartFailure::kNull)
                                    {
                                        return std::unique_ptr<LoggingProtocol>{};
                                    }
                                    return Fail(ErrorKind::kInvalidConfig, "factory failure");
                                });
    }
    DesktopLoggingRun run{engine};
    run.setCsvEnabled(true, directory.path(), "run");
    SignalRecorder completions{&run, &DesktopLoggingRun::sessionEnded};
    bool initialized = false;
    QObject::connect(&engine, &LoggingEngine::logE, &run, [&](const QString&, bool, bool)
                     { initialized = run.view().active_snapshot.has_value() && run.view().csv_open; });
    const auto expected = failure == StartFailure::kThrow || failure == StartFailure::kNull ? ErrorKind::kInternal
                                                                                            : ErrorKind::kInvalidConfig;
    EXPECT_THAT(run.start(makeRequest(directory.path())), IsErr(expected));
    EXPECT_TRUE(initialized);
    EXPECT_FALSE(run.view().active_snapshot.has_value());
    EXPECT_FALSE(run.view().csv_open);
    EXPECT_TRUE(run.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_FALSE(engine.IsRunning());
    EXPECT_EQ(completions.Count(), 0U);
}
INSTANTIATE_TEST_SUITE_P(Failures, DesktopLoggingRunStartFailure,
                         ::testing::Values(StartFailure::kMissingRegistration, StartFailure::kFactory,
                                           StartFailure::kThrow, StartFailure::kNull));

enum class EndCase
{
    kHandshake,
    kCancel,
    kAdapter,
    kRuntime
};
class DesktopLoggingRunCompletion : public ::testing::TestWithParam<EndCase>
{
};
TEST_P(DesktopLoggingRunCompletion, engineCompletionReasonsReleaseRunAndCsv)
{
    QTemporaryDir directory;
    LoggingEngine engine;
    const auto end = GetParam();
    engine.RegisterProtocol(
        "SSM",
        [end](const auto&)
        {
            auto source = std::make_unique<ScriptedLoggingProtocol>();
            if (end == EndCase::kHandshake)
            {
                source->QueueStartResult(Fail(ErrorKind::kBadResponse, "handshake"));
            }
            else if (end == EndCase::kCancel)
            {
                source->BlockPollUntilCancelled();
            }
            else
            {
                source->QueuePollResult(
                    Fail(end == EndCase::kAdapter ? ErrorKind::kDisconnected : ErrorKind::kInternal, "poll failure"));
            }
            return source;
        });
    DesktopLoggingRun run{engine};
    run.setCsvEnabled(true, directory.path(), "run");
    SignalRecorder completions{&run, &DesktopLoggingRun::sessionEnded};
    QObject::connect(&run, &DesktopLoggingRun::sessionEnded, &run,
                     [&](SessionEndReason, const QString&)
                     {
                         EXPECT_FALSE(run.view().active_snapshot.has_value());
                         EXPECT_FALSE(run.view().csv_open);
                         EXPECT_FALSE(run.view().acquisition_running);
                     });
    ASSERT_THAT(run.start(makeRequest(directory.path())), IsOk());
    const auto path = run.view().csv_path;
    if (end == EndCase::kCancel)
    {
        run.stop();
    }
    ASSERT_TRUE(WaitUntil([&] { return completions.Count() == 1; }, 2000ms));
    const auto expected = end == EndCase::kHandshake ? SessionEndReason::kHandshakeFailed
                          : end == EndCase::kCancel  ? SessionEndReason::kStoppedByUser
                          : end == EndCase::kAdapter ? SessionEndReason::kAdapterDisconnected
                                                     : SessionEndReason::kRuntimeFailed;
    EXPECT_EQ(std::get<0>(completions.Snapshot().front()), expected);
    EXPECT_EQ(run.view().csv_path, path);
    EXPECT_TRUE(QFile::exists(path));
}
INSTANTIATE_TEST_SUITE_P(Reasons, DesktopLoggingRunCompletion,
                         ::testing::Values(EndCase::kHandshake, EndCase::kCancel, EndCase::kAdapter,
                                           EndCase::kRuntime));

TEST_F(DesktopLoggingRunTest, workerFirstSampleIsRetained)
{
    engine_.RegisterProtocol(
        "SSM",
        [](const auto&)
        {
            auto source = std::make_unique<ScriptedLoggingProtocol>();
            source->QueuePollResult(
                PollData{.responded = true, .samples = {{.channel_id = "parameter:rpm", .raw_value = "42"}}});
            return source;
        });
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path())), IsOk());
    ASSERT_TRUE(WaitUntil([&] { return run_.view().values.ParameterValue("SSM", "rpm") == "42"; }, 2000ms));
    EXPECT_TRUE(readFile(run_.view().csv_path).contains(",42,\n"));
}
TEST_F(DesktopLoggingRunTest, csvUsesCapturedProtocolIdentityAcrossRepeatedPositions)
{
    registerBlocked(engine_, "CDBG");
    auto request = makeRequest(directory_.path(), LoggingTarget::kEcu, "CDBG", "Correct CDBG");
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(request), IsOk());
    request = makeRequest(directory_.path(), LoggingTarget::kTcu, "SSM", "Wrong SSM");
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 22}});
    const auto text = readFile(run_.view().csv_path);
    EXPECT_TRUE(text.startsWith("Time,Correct CDBG,coolant\n"));
    EXPECT_TRUE(text.contains(",22,\n"));
    EXPECT_FALSE(text.contains("Wrong SSM"));
    EXPECT_EQ(text.count("Correct CDBG"), 1);
}
TEST_F(DesktopLoggingRunTest, csvEscapesNamesAndValues)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(makeRequest(directory_.path(), LoggingTarget::kEcu, "SSM", "RPM, \"speed\"\nvalue")),
                IsOk());
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    const auto text = readFile(run_.view().csv_path);
    EXPECT_TRUE(text.startsWith("Time,\"RPM, \"\"speed\"\"\nvalue\",coolant\n"));
    EXPECT_TRUE(text.contains(",42,\n"));
}
} // namespace
} // namespace fastecu::desktop::logging
namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
}
