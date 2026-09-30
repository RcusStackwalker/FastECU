#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_settings.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <QString>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <array>
#include <memory>
#include <vector>

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::ManualCancellationToken;
using fastecu::Status;

namespace
{
class QtPortEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        static int argc = 1;
        static auto program = std::to_array("qt_port_adapters_test");
        static auto argv = std::to_array<char *>({program.data(), nullptr});
        app_ = std::make_unique<QCoreApplication>(argc, argv.data());
        QCoreApplication::setOrganizationName("FastECU-test");
        QCoreApplication::setApplicationName("qt-port-adapters-test");
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }

  private:
    QTemporaryDir settings_dir_;
    std::unique_ptr<QCoreApplication> app_;
};

const auto *qt_port_environment = ::testing::AddGlobalTestEnvironment(new QtPortEnvironment);
} // namespace

// ---- QtClock ---------------------------------------------------------

TEST(QtClockTest, NowMsIsMonotonicNonDecreasing)
{
    QtClock clock;
    auto first = clock.now();
    ManualCancellationToken token;
    ASSERT_THAT(clock.sleep(1ms, token), fastecu::testing::IsOk());
    auto second = clock.now();
    EXPECT_GE(second, first);
}

TEST(QtClockTest, SleepZeroSucceeds)
{
    QtClock clock;
    ManualCancellationToken token;
    EXPECT_THAT(clock.sleep(0ms, token), fastecu::testing::IsOk());
}

TEST(QtClockTest, SleepReturnsCancelledWhenTokenAlreadyCancelled)
{
    QtClock clock;
    ManualCancellationToken token;
    token.cancel();
    ASSERT_THAT(clock.sleep(50ms, token), fastecu::testing::IsErr(ErrorKind::Cancelled));
}

// ---- QtFileRepository --------------------------------------------------

TEST(QtFileRepositoryTest, WriteThenReadRoundTripsBytes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string path = dir.filePath("payload.bin").toStdString();

    QtFileRepository repo;
    std::vector<std::uint8_t> data{0x00, 0x01, 0x7f, 0x80, 0xff, 'h', 'i'};
    ASSERT_THAT(repo.write(path, std::span<const std::uint8_t>(data)), fastecu::testing::IsOk());

    ASSERT_THAT(repo.read(path), fastecu::testing::IsOkAnd(data));
}

TEST(QtFileRepositoryTest, WriteReportsSuccessOnlyOnceBytesAreOnDisk)
{
    // write() now flushes and closes before returning, so a successful Status
    // means the file is complete on disk -- not merely handed to QFile's
    // buffer. A failure that only surfaces at flush/close (a full volume is
    // the classic case) must not be reported as a successful write.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString path = dir.filePath("flushed.bin");

    QtFileRepository repo;
    std::vector<std::uint8_t> data(4096, 0xA5);
    ASSERT_THAT(repo.write(path.toStdString(), std::span<const std::uint8_t>(data)), fastecu::testing::IsOk());
    EXPECT_EQ(QFileInfo(path).size(), static_cast<qint64>(data.size()));
}

TEST(QtFileRepositoryTest, WriteToUnopenablePathFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // The parent directory does not exist, so the file cannot be opened for
    // writing at all.
    std::string path = dir.filePath("no-such-directory/payload.bin").toStdString();

    QtFileRepository repo;
    std::vector<std::uint8_t> data{0x01, 0x02, 0x03};
    ASSERT_THAT(repo.write(path, std::span<const std::uint8_t>(data)),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(QtFileRepositoryTest, ReadOfMissingPathFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string path = dir.filePath("does-not-exist.bin").toStdString();

    QtFileRepository repo;
    ASSERT_THAT(repo.read(path), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

// ---- QtSettings ---------------------------------------------------------
// Isolation: QtPortEnvironment redirects QSettings to a throwaway temp
// directory before any QtSettings is constructed, so these tests never touch
// the real user config.

TEST(QtSettingsTest, GetOfMissingKeyReturnsNullopt)
{
    QtSettings settings;
    EXPECT_EQ(settings.get("qt-port-adapters-test/missing-key"), std::nullopt);
}

TEST(QtSettingsTest, SetThenGetRoundTrips)
{
    QtSettings settings;
    settings.set("qt-port-adapters-test/round-trip", "some-value");
    std::optional<std::string> v = settings.get("qt-port-adapters-test/round-trip");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "some-value");
}

// ---- QtEventSink --------------------------------------------------------

TEST(QtEventSinkTest, LogEmitsLoggedWithConvertedArgs)
{
    QtEventSink sink;
    fastecu::testing::SignalRecorder spy(&sink, &QtEventSink::logged);
    ASSERT_TRUE(spy.is_valid());

    sink.log(LogLevel::Warning, "msg");

    ASSERT_EQ(spy.count(), 1);
    const auto args = spy.snapshot().front();
    EXPECT_EQ(std::get<0>(args), static_cast<int>(LogLevel::Warning));
    EXPECT_EQ(std::get<1>(args), QString("msg"));
}

TEST(QtEventSinkTest, ProgressEmitsProgressedWithDoneAndTotal)
{
    QtEventSink sink;
    fastecu::testing::SignalRecorder spy(&sink, &QtEventSink::progressed);
    ASSERT_TRUE(spy.is_valid());

    sink.progress(3, 10);

    ASSERT_EQ(spy.count(), 1);
    const auto args = spy.snapshot().front();
    EXPECT_EQ(std::get<0>(args), 3);
    EXPECT_EQ(std::get<1>(args), 10);
}

TEST(QtEventSinkTest, PhaseProgressPreservesLegacyProgressAndConvertsPhaseName)
{
    QtEventSink sink;
    fastecu::testing::SignalRecorder legacySpy(&sink, &QtEventSink::progressed);
    fastecu::testing::SignalRecorder phaseSpy(&sink, &QtEventSink::phaseProgressed);
    ASSERT_TRUE(legacySpy.is_valid());
    ASSERT_TRUE(phaseSpy.is_valid());

    sink.phase_progress({.phase_name = "Write userspace", .phase_index = 4, .phase_count = 6, .done = 3, .total = 10});

    ASSERT_EQ(legacySpy.count(), 1);
    EXPECT_EQ(std::get<0>(legacySpy.snapshot().at(0)), 3);
    EXPECT_EQ(std::get<1>(legacySpy.snapshot().at(0)), 10);
    ASSERT_EQ(phaseSpy.count(), 1);
    EXPECT_EQ(std::get<0>(phaseSpy.snapshot().at(0)), QString("Write userspace"));
    EXPECT_EQ(std::get<1>(phaseSpy.snapshot().at(0)), 4);
    EXPECT_EQ(std::get<2>(phaseSpy.snapshot().at(0)), 6);
    EXPECT_EQ(std::get<3>(phaseSpy.snapshot().at(0)), 3);
    EXPECT_EQ(std::get<4>(phaseSpy.snapshot().at(0)), 10);
}

TEST(QtEventSinkTest, NoticeEmitsNoticedWithMessage)
{
    QtEventSink sink;
    fastecu::testing::SignalRecorder spy(&sink, &QtEventSink::noticed);
    ASSERT_TRUE(spy.is_valid());

    sink.notice("done");

    ASSERT_EQ(spy.count(), 1);
    const auto args = spy.snapshot().front();
    EXPECT_EQ(std::get<0>(args), QString("done"));
}
