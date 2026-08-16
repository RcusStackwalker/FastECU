#include "src/platform/desktop/common/ports/qt_cancellation_token.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_settings.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::Result;
using fastecu::Status;

namespace
{
class QtPortEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        static int argc = 1;
        static char program[] = "qt_port_adapters_test";
        static char *argv[] = {program, nullptr};
        app_ = std::make_unique<QCoreApplication>(argc, argv);
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
    std::uint64_t first = clock.now_ms();
    QtCancellationToken token;
    Status s = clock.sleep(1, token);
    ASSERT_TRUE(s.has_value());
    std::uint64_t second = clock.now_ms();
    EXPECT_GE(second, first);
}

TEST(QtClockTest, SleepZeroSucceeds)
{
    QtClock clock;
    QtCancellationToken token;
    Status s = clock.sleep(0, token);
    EXPECT_TRUE(s.has_value());
}

TEST(QtClockTest, SleepReturnsCancelledWhenTokenAlreadyCancelled)
{
    QtClock clock;
    QtCancellationToken token;
    token.cancel();
    Status s = clock.sleep(50, token);
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().kind, ErrorKind::Cancelled);
}

// ---- QtCancellationToken ----------------------------------------------

TEST(QtCancellationTokenTest, FreshTokenIsNotCancelled)
{
    QtCancellationToken token;
    EXPECT_FALSE(token.cancelled());
}

TEST(QtCancellationTokenTest, CancelSetsFlag)
{
    QtCancellationToken token;
    token.cancel();
    EXPECT_TRUE(token.cancelled());
}

TEST(QtCancellationTokenTest, ResetClearsFlag)
{
    QtCancellationToken token;
    token.cancel();
    ASSERT_TRUE(token.cancelled());
    token.reset();
    EXPECT_FALSE(token.cancelled());
}

// ---- QtFileRepository --------------------------------------------------

TEST(QtFileRepositoryTest, WriteThenReadRoundTripsBytes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string path = dir.filePath("payload.bin").toStdString();

    QtFileRepository repo;
    std::vector<std::uint8_t> data{0x00, 0x01, 0x7f, 0x80, 0xff, 'h', 'i'};
    Status w = repo.write(path, std::span<const std::uint8_t>(data));
    ASSERT_TRUE(w.has_value());

    Result<std::vector<std::uint8_t>> r = repo.read(path);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, data);
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
    Status w = repo.write(path.toStdString(), std::span<const std::uint8_t>(data));
    ASSERT_TRUE(w.has_value());
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
    Status w = repo.write(path, std::span<const std::uint8_t>(data));
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error().kind, ErrorKind::InvalidConfig);
}

TEST(QtFileRepositoryTest, ReadOfMissingPathFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string path = dir.filePath("does-not-exist.bin").toStdString();

    QtFileRepository repo;
    Result<std::vector<std::uint8_t>> r = repo.read(path);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::InvalidConfig);
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
    QSignalSpy spy(&sink, &QtEventSink::logged);
    ASSERT_TRUE(spy.isValid());

    sink.log(LogLevel::Warning, "msg");

    ASSERT_EQ(spy.count(), 1);
    QList<QVariant> args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), static_cast<int>(LogLevel::Warning));
    EXPECT_EQ(args.at(1).toString(), QString("msg"));
}

TEST(QtEventSinkTest, ProgressEmitsProgressedWithDoneAndTotal)
{
    QtEventSink sink;
    QSignalSpy spy(&sink, &QtEventSink::progressed);
    ASSERT_TRUE(spy.isValid());

    sink.progress(3, 10);

    ASSERT_EQ(spy.count(), 1);
    QList<QVariant> args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), 3);
    EXPECT_EQ(args.at(1).toInt(), 10);
}

TEST(QtEventSinkTest, PhaseProgressPreservesLegacyProgressAndConvertsPhaseName)
{
    QtEventSink sink;
    QSignalSpy legacySpy(&sink, &QtEventSink::progressed);
    QSignalSpy phaseSpy(&sink, &QtEventSink::phaseProgressed);
    ASSERT_TRUE(legacySpy.isValid());
    ASSERT_TRUE(phaseSpy.isValid());

    sink.phase_progress({.phase_name = "Write userspace", .phase_index = 4, .phase_count = 6, .done = 3, .total = 10});

    ASSERT_EQ(legacySpy.count(), 1);
    EXPECT_EQ(legacySpy.at(0).at(0).toInt(), 3);
    EXPECT_EQ(legacySpy.at(0).at(1).toInt(), 10);
    ASSERT_EQ(phaseSpy.count(), 1);
    EXPECT_EQ(phaseSpy.at(0).at(0).toString(), QString("Write userspace"));
    EXPECT_EQ(phaseSpy.at(0).at(1).toInt(), 4);
    EXPECT_EQ(phaseSpy.at(0).at(2).toInt(), 6);
    EXPECT_EQ(phaseSpy.at(0).at(3).toInt(), 3);
    EXPECT_EQ(phaseSpy.at(0).at(4).toInt(), 10);
}

TEST(QtEventSinkTest, NoticeEmitsNoticedWithMessage)
{
    QtEventSink sink;
    QSignalSpy spy(&sink, &QtEventSink::noticed);
    ASSERT_TRUE(spy.isValid());

    sink.notice("done");

    ASSERT_EQ(spy.count(), 1);
    QList<QVariant> args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toString(), QString("done"));
}
