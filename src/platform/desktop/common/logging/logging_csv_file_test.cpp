#include "src/platform/desktop/common/logging/logging_csv_file.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/logging/testing/failing_csv_storage.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

namespace fastecu::desktop::logging
{
namespace
{
using namespace std::chrono_literals;
using namespace fastecu::logging;
Result<LoggingRunSnapshot> Snapshot()
{
    LoggerModel model;
    model.InstallDefinition(
        {.parameters = {{.protocol = "SSM",
                         .id = "rpm",
                         .name = "RPM, speed",
                         .address = "10",
                         .length = "1",
                         .enabled = true,
                         .conversions = {{.units = "rpm", .expr = "x", .format = "0.00"}}}},
         .switches = {{.protocol = "SSM", .id = "rpm", .name = "Flag", .address = "20", .sample_bit = "5"}}});
    model.SetSelection({.protocol = "SSM", .gauge_ids = {"rpm"}, .lower_panel_ids = {"rpm"}, .switch_ids = {"rpm"}});
    return PrepareLoggingRun(model, LoggingProtocolId::kSsm, "SSM",
                             {.poll_timeout = 50ms,
                              .car_silence_miss_threshold = 2,
                              .reconnect_attempt_threshold = 3,
                              .reconnect_retry_period = 0},
                             LoggingTarget::kEcu);
}
QByteArray Read(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
using testing::FailingCsvStorage;
TEST(LoggingCsvFileTest, IdenticalRapidRunsAndExistingNamesPreserveCompletedOutput)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto run = Snapshot();
    ASSERT_THAT(run, fastecu::testing::IsOk());
    DesktopLoggerValues values;
    values.BeginRun(*run);
    ASSERT_TRUE(values.SetParameterValue({"SSM", "rpm"}, "42.00"));
    ASSERT_TRUE(values.SetSwitchValue({"SSM", "rpm"}, "1"));
    QFile existing(QDir(directory.path()).filePath("run.csv"));
    ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
    ASSERT_EQ(existing.write("preserve"), 8);
    existing.close();
    LoggingCsvFile writer;
    const auto first = writer.BeginRun(*run, directory.path(), "run");
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_THAT(writer.AppendRow(values, 1234ms), fastecu::testing::IsOk());
    ASSERT_THAT(writer.EndRun(), fastecu::testing::IsOk());
    const auto first_bytes = Read(*first);
    EXPECT_EQ(first_bytes, QByteArray("Time,\"RPM, speed\",\"RPM, speed\",Flag,\n1.234,42.00,42.00,1,\n"));
    const auto second = writer.BeginRun(*run, directory.path(), "run");
    ASSERT_THAT(second, fastecu::testing::IsOk());
    EXPECT_NE(*first, *second);
    values.BeginRun(*run);
    ASSERT_THAT(writer.AppendRow(values, 0ms), fastecu::testing::IsOk());
    ASSERT_THAT(writer.EndRun(), fastecu::testing::IsOk());
    EXPECT_EQ(Read(*first), first_bytes);
    EXPECT_EQ(Read(existing.fileName()), "preserve");
    EXPECT_EQ(Read(*second), QByteArray("Time,\"RPM, speed\",\"RPM, speed\",Flag,\n0,,,,\n"));
    EXPECT_FALSE(writer.IsOpen());
    EXPECT_THAT(writer.EndRun(), fastecu::testing::IsOk());
}
TEST(LoggingCsvFileTest, CreationFailureDoesNotClaimOwnership)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto run = Snapshot();
    ASSERT_THAT(run, fastecu::testing::IsOk());
    LoggingCsvFile writer;
    EXPECT_THAT(writer.BeginRun(*run, directory.path() + "/absent", "run"),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_FALSE(writer.IsOpen());
}
TEST(LoggingCsvFileTest, WriteFailureReleasesOwnershipAndReturnsAnError)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto run = Snapshot();
    ASSERT_THAT(run, fastecu::testing::IsOk());
    auto storage = std::make_unique<FailingCsvStorage>();
    auto *injected = storage.Get();
    LoggingCsvFile writer_(std::move(storage));
    const auto path = writer.BeginRun(*run, directory.path(), "run");
    ASSERT_THAT(path, fastecu::testing::IsOk());
    DesktopLoggerValues values;
    values.BeginRun(*run);
    injected->fail_writes = true;
    EXPECT_THAT(writer.AppendRow(values, 0ms), fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_FALSE(writer.IsOpen());
    EXPECT_EQ(Read(*path), QByteArray("Time,\"RPM, speed\",\"RPM, speed\",Flag,\n"));
}
} // namespace
} // namespace fastecu::desktop::logging
