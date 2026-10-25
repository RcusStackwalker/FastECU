#include <array>
#include "src/ui/desktop/widgets/desktop_logging_run.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <gmock/gmock-matchers.h>
#include <gmock/gmock.h>
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
using ::testing::_;
using ::testing::Eq;
using ::testing::Optional;

Result<DesktopLoggingRunStart> makeRequest(const QString& directory, LoggingTarget target = LoggingTarget::kTcu,
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
    auto snapshot = fastecu::logging::PrepareLoggingRun(
        model, protocol == "CDBG" ? LoggingProtocolId::kCdbg : LoggingProtocolId::kSsm, protocol,
        {.poll_timeout = 50ms,
         .car_silence_miss_threshold = 100000,
         .reconnect_attempt_threshold = 100001,
         .reconnect_retry_period = 0},
        target);
    if (!snapshot.has_value())
    {
        return std::unexpected(snapshot.error());
    }
    return DesktopLoggingRunStart{{QString::fromStdString(protocol)}, std::move(*snapshot), directory, "run"};
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
    auto builtRequest1 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest1, fastecu::testing::IsOk());
    auto request = std::move(*builtRequest1);
    ASSERT_THAT(run_.start(request), IsOk());
    auto builtRequest2 = makeRequest(directory_.path(), LoggingTarget::kEcu);
    ASSERT_THAT(builtRequest2, fastecu::testing::IsOk());
    request = std::move(*builtRequest2);
    const auto view = run_.view();
    ASSERT_THAT(view.active_snapshot, Optional(_));
    EXPECT_EQ(view.active_snapshot->Target(), LoggingTarget::kTcu);
    EXPECT_THAT(view.active_snapshot->Selection().lower_panel_ids, ::testing::ElementsAre("rpm", "coolant"));
    EXPECT_TRUE(view.values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_TRUE(view.acquisition_running);
}
TEST_F(DesktopLoggingRunTest, unavailableSampleClearsOnlyItsCachedValueAndHealthyPeerContinues)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest3 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest3, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest3)), IsOk());
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
    auto builtRequest4 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest4, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest4)), IsOk());
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
                         EXPECT_THAT(run_.view().active_snapshot, Eq(std::nullopt));
                         EXPECT_FALSE(run_.view().csv_open);
                         EXPECT_FALSE(run_.view().acquisition_running);
                     });
    auto builtRequest5 = makeRequest(missing);
    ASSERT_THAT(builtRequest5, fastecu::testing::IsOk());
    EXPECT_THAT(run_.start(std::move(*builtRequest5)), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(errors.Count(), 1U);
}

TEST_F(DesktopLoggingRunTest, secondStartPreservesTheActiveRun)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest6 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest6, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest6)), IsOk());
    const auto before = run_.view();
    auto builtRequest7 = makeRequest(directory_.path(), LoggingTarget::kEcu);
    ASSERT_THAT(builtRequest7, fastecu::testing::IsOk());
    EXPECT_THAT(run_.start(std::move(*builtRequest7)), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(run_.view().active_snapshot->Target(), before.active_snapshot->Target());
    EXPECT_EQ(run_.view().csv_path, before.csv_path);
    EXPECT_TRUE(run_.view().acquisition_running);
}
TEST_F(DesktopLoggingRunTest, sampleErrorSubscriberStopsBeforeHealthyPeer)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest8 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest8, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest8)), IsOk());
    const auto path = run_.view().csv_path;
    SignalRecorder errors{&run_, &DesktopLoggingRun::sampleError};
    QObject::connect(&run_, &DesktopLoggingRun::sampleError, &run_, [&](const QString&) { run_.stop(); });
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_EQ(errors.Count(), 1U);
    EXPECT_THAT(run_.view().active_snapshot, Eq(std::nullopt));
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_EQ(readFile(path), "Time,rpm,coolant\n");
}
TEST_F(DesktopLoggingRunTest, replacementDuringSampleErrorDiscardsOldBatch)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest9 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest9, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest9)), IsOk());
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
                         auto builtRequest10 = makeRequest(directory_.path(), LoggingTarget::kEcu);
                         ASSERT_THAT(builtRequest10, fastecu::testing::IsOk());
                         ASSERT_THAT(run_.start(std::move(*builtRequest10)), IsOk());
                     });
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_TRUE(replaced);
    ASSERT_THAT(run_.view().active_snapshot, Optional(_));
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
    auto builtRequest11 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest11, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest11)), IsOk());
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
                         auto builtRequest12 = makeRequest(directory_.path());
                         ASSERT_THAT(builtRequest12, fastecu::testing::IsOk());
                         ASSERT_THAT(run_.start(std::move(*builtRequest12)), IsOk());
                     });
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_TRUE(replaced);
    ASSERT_THAT(run_.view().active_snapshot, Optional(_));
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
    auto builtRequest13 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest13, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest13)), IsOk());
    const auto path = run_.view().csv_path;
    SignalRecorder presentations{&run_, &DesktopLoggingRun::presentationChanged};
    SignalRecorder errors{&run_, &DesktopLoggingRun::sampleError};
    QObject::connect(&run_, &DesktopLoggingRun::sampleError, &run_, [&](const QString&) { run_.shutdown(); });
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 99}});
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 43}});
    EXPECT_EQ(errors.Count(), 1U);
    EXPECT_EQ(presentations.Count(), 0U);
    EXPECT_THAT(run_.view().active_snapshot, Eq(std::nullopt));
    EXPECT_FALSE(run_.view().acquisition_running);
    EXPECT_FALSE(run_.view().csv_open);
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_EQ(readFile(path), "Time,rpm,coolant\n");
}
TEST_F(DesktopLoggingRunTest, failedReplacementDoesNotResumeOldBatch)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest14 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest14, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest14)), IsOk());
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
                         auto builtRequest15 = makeRequest(directory_.path());
                         ASSERT_THAT(builtRequest15, fastecu::testing::IsOk());
                         EXPECT_THAT(run_.start(std::move(*builtRequest15)), IsErr(ErrorKind::kInvalidConfig));
                     });
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 99}});
    EXPECT_TRUE(attempted);
    EXPECT_EQ(presentations.Count(), 0U);
    EXPECT_THAT(run_.view().active_snapshot, Eq(std::nullopt));
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
    auto builtRequest16 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest16, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest16)), IsOk());
    SignalRecorder errors{&run_, &DesktopLoggingRun::sampleError};
    emit engine_.valuesUpdated(
        {{.channel_id = "unknown", .numeric_value = 8}, {.channel_id = "parameter:rpm", .numeric_value = 43}});
    EXPECT_EQ(errors.Count(), 1U);
    EXPECT_THAT(run_.view().active_snapshot, Optional(_));
    EXPECT_TRUE(run_.view().acquisition_running);
    EXPECT_EQ(run_.view().values.ParameterValue("SSM", "rpm"), "43");
    EXPECT_TRUE(readFile(run_.view().csv_path).contains(",43,\n"));
}
TEST_F(DesktopLoggingRunTest, stopDuringPresentationDiscardsPendingCsvRow)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest17 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest17, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest17)), IsOk());
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
    EXPECT_THAT(run_.view().active_snapshot, Eq(std::nullopt));
    EXPECT_FALSE(run_.view().csv_open);
    EXPECT_EQ(readFile(path), "Time,rpm,coolant\n");
}
TEST_F(DesktopLoggingRunTest, copiedViewSurvivesStopAndRestart)
{
    auto builtRequest18 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest18, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest18)), IsOk());
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    const auto saved = run_.view();
    run_.stop();
    auto builtRequest19 = makeRequest(directory_.path(), LoggingTarget::kEcu);
    ASSERT_THAT(builtRequest19, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest19)), IsOk());
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 43}});
    EXPECT_EQ(saved.active_snapshot->Target(), LoggingTarget::kTcu);
    EXPECT_EQ(saved.values.ParameterValue("SSM", "rpm"), "42");
    EXPECT_EQ(run_.view().values.ParameterValue("SSM", "rpm"), "43");
}
TEST_F(DesktopLoggingRunTest, completionReleasesOutputBeforeAnImmediateRestart)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest20 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest20, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest20)), IsOk());
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
                             EXPECT_THAT(run_.view().active_snapshot, Eq(std::nullopt));
                             EXPECT_FALSE(run_.view().csv_open);
                             EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
                             auto builtRequest21 = makeRequest(directory_.path(), LoggingTarget::kEcu);
                             ASSERT_THAT(builtRequest21, fastecu::testing::IsOk());
                             ASSERT_THAT(run_.start(std::move(*builtRequest21)), IsOk());
                         });
    run_.stop();
    QObject::disconnect(subscription);
    EXPECT_TRUE(restarted);
    ASSERT_THAT(run_.view().active_snapshot, Optional(_));
    EXPECT_EQ(run_.view().active_snapshot->Target(), LoggingTarget::kEcu);
    EXPECT_TRUE(run_.view().csv_open);
}
TEST_F(DesktopLoggingRunTest, shutdownClosesOutputAndUsesTheNonModalOutcomePath)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest22 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest22, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest22)), IsOk());
    SignalRecorder errors{&run_, &DesktopLoggingRun::csvError};
    SignalRecorder completions{&run_, &DesktopLoggingRun::sessionEnded};
    run_.shutdown();
    run_.shutdown();
    EXPECT_FALSE(engine_.IsRunning());
    EXPECT_THAT(run_.view().active_snapshot, Eq(std::nullopt));
    EXPECT_FALSE(run_.view().csv_open);
    EXPECT_TRUE(run_.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_EQ(errors.Count(), 0U);
    EXPECT_EQ(completions.Count(), 0U);
    auto builtRequest23 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest23, fastecu::testing::IsOk());
    EXPECT_THAT(run_.start(std::move(*builtRequest23)), IsErr(ErrorKind::kInvalidConfig));
}
TEST_F(DesktopLoggingRunTest, disablingCsvClosesOnlyOutputAndReenableUsesCapturedSchema)
{
    run_.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest24 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest24, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest24)), IsOk());
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
    auto builtRequest25 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest25, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest25)), IsOk());
    QThread::msleep(500);
    emit engine_.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    const auto first = run_.view().csv_path;
    const auto bytes = readFile(first);
    run_.stop();
    auto builtRequest26 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest26, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest26)), IsOk());
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
                         EXPECT_THAT(run.view().active_snapshot, Optional(_));
                         EXPECT_TRUE(run.view().acquisition_running);
                         EXPECT_FALSE(run.view().csv_open);
                     });
    auto builtRequest27 = makeRequest(destination);
    ASSERT_THAT(builtRequest27, fastecu::testing::IsOk());
    ASSERT_THAT(run.start(std::move(*builtRequest27)), IsOk());
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
    auto builtRequest28 = makeRequest(destination);
    ASSERT_THAT(builtRequest28, fastecu::testing::IsOk());
    ASSERT_THAT(run.start(std::move(*builtRequest28)), IsOk());
    injected->fail_writes = GetParam() != FailurePoint::kOpen;
    emit engine.valuesUpdated({{.channel_id = "parameter:rpm", .numeric_value = 42}});
    const auto first = run.view().csv_path;
    run.stop();
    injected->fail_writes = false;
    auto builtRequest29 = makeRequest(directory.path());
    ASSERT_THAT(builtRequest29, fastecu::testing::IsOk());
    ASSERT_THAT(run.start(std::move(*builtRequest29)), IsOk());
    EXPECT_TRUE(run.view().csv_enabled);
    EXPECT_TRUE(run.view().csv_open);
    EXPECT_NE(first, run.view().csv_path);
    EXPECT_TRUE(readFile(run.view().csv_path).startsWith("Time,rpm,coolant\n"));
    EXPECT_EQ(errors.Count(), 1U);
}
INSTANTIATE_TEST_SUITE_P(Failures, DesktopLoggingRunFailure,
                         ::testing::Values(FailurePoint::kOpen, FailurePoint::kHeaderWrite, FailurePoint::kRowWrite),
                         (
                             [](const auto& paramInfo)
                             {
                                 static constexpr std::array kNames{"Open", "HeaderWrite", "RowWrite"};
                                 return kNames[paramInfo.index];
                             }));

TEST_F(DesktopLoggingRunTest, csvErrorSubscriberStopsAfterFailureStateCommits)
{
    auto storage = std::make_unique<testing::FailingCsvStorage>();
    auto *injected = storage.get();
    DesktopLoggingRun run{engine_, LoggingCsvFile{std::move(storage)}};
    run.setCsvEnabled(true, directory_.path(), "run");
    auto builtRequest30 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest30, fastecu::testing::IsOk());
    ASSERT_THAT(run.start(std::move(*builtRequest30)), IsOk());
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
    EXPECT_THAT(run.view().active_snapshot, Eq(std::nullopt));
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
    auto builtRequest31 = makeRequest(directory.path());
    ASSERT_THAT(builtRequest31, fastecu::testing::IsOk());
    EXPECT_THAT(run.start(std::move(*builtRequest31)), IsErr(expected));
    EXPECT_TRUE(initialized);
    EXPECT_THAT(run.view().active_snapshot, Eq(std::nullopt));
    EXPECT_FALSE(run.view().csv_open);
    EXPECT_TRUE(run.view().values.ParameterValue("SSM", "rpm").isEmpty());
    EXPECT_FALSE(engine.IsRunning());
    EXPECT_EQ(completions.Count(), 0U);
}
INSTANTIATE_TEST_SUITE_P(Failures, DesktopLoggingRunStartFailure,
                         ::testing::Values(StartFailure::kMissingRegistration, StartFailure::kFactory,
                                           StartFailure::kThrow, StartFailure::kNull),
                         (
                             [](const auto& paramInfo)
                             {
                                 static constexpr std::array kNames{"MissingRegistration", "Factory", "Throw", "Null"};
                                 return kNames[paramInfo.index];
                             }));

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
                         EXPECT_THAT(run.view().active_snapshot, Eq(std::nullopt));
                         EXPECT_FALSE(run.view().csv_open);
                         EXPECT_FALSE(run.view().acquisition_running);
                     });
    auto builtRequest32 = makeRequest(directory.path());
    ASSERT_THAT(builtRequest32, fastecu::testing::IsOk());
    ASSERT_THAT(run.start(std::move(*builtRequest32)), IsOk());
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
                         ::testing::Values(EndCase::kHandshake, EndCase::kCancel, EndCase::kAdapter, EndCase::kRuntime),
                         (
                             [](const auto& paramInfo)
                             {
                                 static constexpr std::array kNames{"Handshake", "Cancel", "Adapter", "Runtime"};
                                 return kNames[paramInfo.index];
                             }));

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
    auto builtRequest33 = makeRequest(directory_.path());
    ASSERT_THAT(builtRequest33, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest33)), IsOk());
    ASSERT_TRUE(WaitUntil([&] { return run_.view().values.ParameterValue("SSM", "rpm") == "42"; }, 2000ms));
    EXPECT_TRUE(readFile(run_.view().csv_path).contains(",42,\n"));
}
TEST_F(DesktopLoggingRunTest, csvUsesCapturedProtocolIdentityAcrossRepeatedPositions)
{
    registerBlocked(engine_, "CDBG");
    auto builtRequest34 = makeRequest(directory_.path(), LoggingTarget::kEcu, "CDBG", "Correct CDBG");
    ASSERT_THAT(builtRequest34, fastecu::testing::IsOk());
    auto request = std::move(*builtRequest34);
    run_.setCsvEnabled(true, directory_.path(), "run");
    ASSERT_THAT(run_.start(request), IsOk());
    auto builtRequest35 = makeRequest(directory_.path(), LoggingTarget::kTcu, "SSM", "Wrong SSM");
    ASSERT_THAT(builtRequest35, fastecu::testing::IsOk());
    request = std::move(*builtRequest35);
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
    auto builtRequest36 = makeRequest(directory_.path(), LoggingTarget::kEcu, "SSM", "RPM, \"speed\"\nvalue");
    ASSERT_THAT(builtRequest36, fastecu::testing::IsOk());
    ASSERT_THAT(run_.start(std::move(*builtRequest36)), IsOk());
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
