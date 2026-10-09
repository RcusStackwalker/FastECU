#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/testing/fake_backed_serial.h"
#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_kline_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h"
#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::ICancellationToken;
using ReadResult = fastecu::Result<std::optional<bytes::Bytes>>;

enum class ReadPath
{
    kKline,
    kSsm,
    kKlineFlash,
    kKlineRaw,
    kCanFlash,
    kMixedCan
};

class TransportReads : public ::testing::TestWithParam<ReadPath>
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(mixed_.Configure({}).has_value());
    }

    ReadResult Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation)
    {
        switch (GetParam())
        {
        case ReadPath::kKline:
            return kline_.Read(timeout, cancellation);
        case ReadPath::kSsm:
            return ssm_.Read(timeout, cancellation);
        case ReadPath::kKlineFlash:
            return kline_flash_.Read(timeout, cancellation);
        case ReadPath::kKlineRaw:
            return kline_flash_.ReadRaw(timeout, cancellation);
        case ReadPath::kCanFlash:
            return can_flash_.Read(timeout, cancellation);
        case ReadPath::kMixedCan:
            return mixed_.ReadIso15765(timeout, cancellation);
        }
        return fastecu::Fail(ErrorKind::kInternal, "unknown test read path");
    }

    void ExpectRead(std::uint16_t timeout, std::function<QByteArray(std::uint16_t)> action)
    {
        if (GetParam() == ReadPath::kKlineRaw)
        {
            EXPECT_CALL(serial_.Fake(), ReadSerialData(::testing::_)).Times(0);
            EXPECT_CALL(serial_.Fake(), ReadSerialObdData(timeout)).WillOnce(std::move(action));
        }
        else
        {
            EXPECT_CALL(serial_.Fake(), ReadSerialObdData(::testing::_)).Times(0);
            EXPECT_CALL(serial_.Fake(), ReadSerialData(timeout)).WillOnce(std::move(action));
        }
    }

    bool HasUnblock() const
    {
        return GetParam() != ReadPath::kKline && GetParam() != ReadPath::kSsm;
    }

    void Unblock()
    {
        kline_flash_.RequestUnblock();
        can_flash_.RequestUnblock();
        mixed_.RequestUnblock();
    }

    FakeBackedSerial<> serial_;
    mutdma::FastEcuKlineTransport kline_{serial_.Get()};
    FastEcuSsmTransport ssm_{serial_.Get()};
    fastecu::flash::DesktopKlineFlashTransport kline_flash_{serial_.Get()};
    fastecu::flash::DesktopCanFlashTransport can_flash_{serial_.Get()};
    fastecu::flash::DesktopMixedCanFlashTransport mixed_{serial_.Get()};
    FakeCancellationToken cancellation_;
};

TEST_P(TransportReads, SaturatesTimeoutAndPreservesBytes)
{
    struct Case
    {
        std::chrono::milliseconds timeout;
        std::uint16_t expected;
    };
    for (const Case& test : {Case{-1ms, 0}, Case{0ms, 0}, Case{65535ms, 65535}, Case{65536ms, 65535},
                             Case{std::chrono::milliseconds::max(), 65535}})
    {
        SCOPED_TRACE(test.timeout.count());
        ExpectRead(test.expected, [](std::uint16_t) { return QByteArray("\x00\x80\xff", 3); });
        const auto result = Read(test.timeout, cancellation_);
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value());
        EXPECT_EQ(result->value(), (bytes::Bytes{0x00, 0x80, 0xff}));
        ::testing::Mock::VerifyAndClearExpectations(&serial_.Fake());
    }
}

TEST_P(TransportReads, SilenceIsAnEmptyOptional)
{
    ExpectRead(10, [](std::uint16_t) { return QByteArray{}; });
    const auto result = Read(10ms, cancellation_);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST_P(TransportReads, CancellationPrecedesDisconnectAndSkipsDriver)
{
    cancellation_.SetCancelled(true);
    EXPECT_CALL(serial_.Fake(), IsSerialPortOpen()).Times(0);
    EXPECT_CALL(serial_.Fake(), ReadSerialData(::testing::_)).Times(0);
    EXPECT_CALL(serial_.Fake(), ReadSerialObdData(::testing::_)).Times(0);
    const auto result = Read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
}

TEST_P(TransportReads, DisconnectionBeforeReadSkipsDriver)
{
    EXPECT_CALL(serial_.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial_.Fake(), ReadSerialData(::testing::_)).Times(0);
    EXPECT_CALL(serial_.Fake(), ReadSerialObdData(::testing::_)).Times(0);
    const auto result = Read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

TEST_P(TransportReads, DisconnectionAfterReadDiscardsBytes)
{
    EXPECT_CALL(serial_.Fake(), IsSerialPortOpen())
        .WillOnce(::testing::Return(true))
        .WillOnce(::testing::Return(false));
    ExpectRead(10, [](std::uint16_t) { return QByteArray("\xaa", 1); });
    const auto result = Read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kDisconnected);
}

TEST_P(TransportReads, PostReadCancellationPrecedesDisconnect)
{
    EXPECT_CALL(serial_.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(true));
    ExpectRead(10,
               [this](std::uint16_t)
               {
                   cancellation_.SetCancelled(true);
                   return QByteArray("\xaa", 1);
               });
    const auto result = Read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
}

TEST_P(TransportReads, StandardExceptionPreservesDiagnostic)
{
    ExpectRead(10, [](std::uint16_t) -> QByteArray { throw std::runtime_error("read failed"); });
    const auto result = Read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), (fastecu::Error{ErrorKind::kInternal, "read failed"}));
}

TEST_P(TransportReads, NonstandardExceptionIsContained)
{
    ExpectRead(10,
               [](std::uint16_t) -> QByteArray
               {
                   throw FakeBackendNonStandardFailure{}; // NOLINT(bugprone-std-exception-baseclass): catch-all probe.
               });
    const auto result = Read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kInternal);
    EXPECT_EQ(result.error().detail, "serial driver read exception");
}

TEST_P(TransportReads, CancellationPrecedesStandardException)
{
    ExpectRead(10,
               [this](std::uint16_t) -> QByteArray
               {
                   cancellation_.SetCancelled(true);
                   throw std::runtime_error("read failed");
               });
    const auto result = Read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
}

TEST_P(TransportReads, CancellationPrecedesNonstandardException)
{
    ExpectRead(10,
               [this](std::uint16_t) -> QByteArray
               {
                   cancellation_.SetCancelled(true);
                   throw FakeBackendNonStandardFailure{}; // NOLINT(bugprone-std-exception-baseclass): catch-all probe.
               });
    const auto result = Read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
}

TEST_P(TransportReads, UnblockPreservesInflightBytesAndSuppressesNextRead)
{
    if (!HasUnblock())
    {
        GTEST_SKIP() << "logging adapters have no unblock contract";
    }
    ExpectRead(10,
               [this](std::uint16_t)
               {
                   Unblock();
                   return QByteArray("\xaa", 1);
               });
    const auto inflight = Read(10ms, cancellation_);
    ASSERT_TRUE(inflight.has_value());
    ASSERT_TRUE(inflight->has_value());
    EXPECT_EQ(inflight->value(), bytes::Bytes{0xaa});
    ::testing::Mock::VerifyAndClearExpectations(&serial_.Fake());
    EXPECT_CALL(serial_.Fake(), ReadSerialData(::testing::_)).Times(0);
    EXPECT_CALL(serial_.Fake(), ReadSerialObdData(::testing::_)).Times(0);
    const auto subsequent = Read(10ms, cancellation_);
    ASSERT_FALSE(subsequent.has_value());
    EXPECT_EQ(subsequent.error().kind, ErrorKind::kCancelled);
}

TEST(TransportReadPrechecks, ClosedAdaptersKeepCancellationAndModePrecedence)
{
    FakeCancellationToken cancellation;
    fastecu::flash::DesktopKlineFlashTransport kline{nullptr};
    fastecu::flash::DesktopCanFlashTransport can{nullptr};
    fastecu::flash::DesktopMixedCanFlashTransport mixed{nullptr};
    const auto raw = kline.ReadRaw(10ms, cancellation);
    ASSERT_FALSE(raw.has_value());
    EXPECT_EQ(raw.error(), (fastecu::Error{ErrorKind::kDisconnected, "read_raw() called after close()"}));
    const auto can_closed = can.Read(10ms, cancellation);
    ASSERT_FALSE(can_closed.has_value());
    EXPECT_EQ(can_closed.error(), (fastecu::Error{ErrorKind::kDisconnected, "read() called after close()"}));

    cancellation.SetCancelled(true);
    EXPECT_EQ(kline.ReadRaw(10ms, cancellation).error().kind, ErrorKind::kCancelled);
    EXPECT_EQ(can.Read(10ms, cancellation).error().kind, ErrorKind::kCancelled);
    // Mixed CAN checks its lifecycle/mode before the serial-read precheck.
    const auto mixed_closed = mixed.ReadIso15765(10ms, cancellation);
    ASSERT_FALSE(mixed_closed.has_value());
    EXPECT_EQ(mixed_closed.error(), (fastecu::Error{ErrorKind::kDisconnected, "ISO-15765 read called after close()"}));
}

INSTANTIATE_TEST_SUITE_P(Adapters, TransportReads,
                         ::testing::Values(ReadPath::kKline, ReadPath::kSsm, ReadPath::kKlineFlash, ReadPath::kKlineRaw,
                                           ReadPath::kCanFlash, ReadPath::kMixedCan));

const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
} // namespace
