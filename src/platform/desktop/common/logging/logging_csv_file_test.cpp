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
Result<LoggingRunSnapshot> snapshot()
{
    LoggerModel model;
    model.install_definition(
        {.parameters = {{.protocol = "SSM",
                         .id = "rpm",
                         .name = "RPM, speed",
                         .address = "10",
                         .length = "1",
                         .enabled = true,
                         .conversions = {{.units = "rpm", .expr = "x", .format = "0.00"}}}},
         .switches = {{.protocol = "SSM", .id = "rpm", .name = "Flag", .address = "20", .sample_bit = "5"}}});
    model.set_selection({.protocol = "SSM", .gauge_ids = {"rpm"}, .lower_panel_ids = {"rpm"}, .switch_ids = {"rpm"}});
    return prepare_logging_run(model, LoggingProtocolId::Ssm, "SSM",
                               {.poll_timeout = 50ms,
                                .car_silence_miss_threshold = 2,
                                .reconnect_attempt_threshold = 3,
                                .reconnect_retry_period = 0},
                               LoggingTarget::Ecu);
}
QByteArray read(const QString& path)
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
    const auto run = snapshot();
    ASSERT_THAT(run, fastecu::testing::IsOk());
    DesktopLoggerValues values;
    values.begin_run(*run);
    ASSERT_TRUE(values.set_parameter_value({"SSM", "rpm"}, "42.00"));
    ASSERT_TRUE(values.set_switch_value({"SSM", "rpm"}, "1"));
    QFile existing(QDir(directory.path()).filePath("run.csv"));
    ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
    ASSERT_EQ(existing.write("preserve"), 8);
    existing.close();
    LoggingCsvFile writer;
    const auto first = writer.begin_run(*run, directory.path(), "run");
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_THAT(writer.append_row(values, 1234ms), fastecu::testing::IsOk());
    ASSERT_THAT(writer.end_run(), fastecu::testing::IsOk());
    const auto first_bytes = read(*first);
    EXPECT_EQ(first_bytes, QByteArray("Time,\"RPM, speed\",\"RPM, speed\",Flag,\n1.234,42.00,42.00,1,\n"));
    const auto second = writer.begin_run(*run, directory.path(), "run");
    ASSERT_THAT(second, fastecu::testing::IsOk());
    EXPECT_NE(*first, *second);
    values.begin_run(*run);
    ASSERT_THAT(writer.append_row(values, 0ms), fastecu::testing::IsOk());
    ASSERT_THAT(writer.end_run(), fastecu::testing::IsOk());
    EXPECT_EQ(read(*first), first_bytes);
    EXPECT_EQ(read(existing.fileName()), "preserve");
    EXPECT_EQ(read(*second), QByteArray("Time,\"RPM, speed\",\"RPM, speed\",Flag,\n0,,,,\n"));
    EXPECT_FALSE(writer.is_open());
    EXPECT_THAT(writer.end_run(), fastecu::testing::IsOk());
}
TEST(LoggingCsvFileTest, CreationFailureDoesNotClaimOwnership)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto run = snapshot();
    ASSERT_THAT(run, fastecu::testing::IsOk());
    LoggingCsvFile writer;
    EXPECT_THAT(writer.begin_run(*run, directory.path() + "/absent", "run"),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_FALSE(writer.is_open());
}
TEST(LoggingCsvFileTest, WriteFailureReleasesOwnershipAndReturnsAnError)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto run = snapshot();
    ASSERT_THAT(run, fastecu::testing::IsOk());
    auto storage = std::make_unique<FailingCsvStorage>();
    auto *injected = storage.get();
    LoggingCsvFile writer(std::move(storage));
    const auto path = writer.begin_run(*run, directory.path(), "run");
    ASSERT_THAT(path, fastecu::testing::IsOk());
    DesktopLoggerValues values;
    values.begin_run(*run);
    injected->fail_writes = true;
    EXPECT_THAT(writer.append_row(values, 0ms), fastecu::testing::IsErr(ErrorKind::Internal));
    EXPECT_FALSE(writer.is_open());
    EXPECT_EQ(read(*path), QByteArray("Time,\"RPM, speed\",\"RPM, speed\",Flag,\n"));
}
} // namespace
} // namespace fastecu::desktop::logging
