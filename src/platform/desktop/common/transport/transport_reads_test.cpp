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
    Kline,
    Ssm,
    KlineFlash,
    KlineRaw,
    CanFlash,
    MixedCan
};

class TransportReads : public ::testing::TestWithParam<ReadPath>
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(mixed_.configure({}).has_value());
    }

    ReadResult read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation)
    {
        switch (GetParam())
        {
        case ReadPath::Kline:
            return kline_.read(timeout, cancellation);
        case ReadPath::Ssm:
            return ssm_.read(timeout, cancellation);
        case ReadPath::KlineFlash:
            return kline_flash_.read(timeout, cancellation);
        case ReadPath::KlineRaw:
            return kline_flash_.read_raw(timeout, cancellation);
        case ReadPath::CanFlash:
            return can_flash_.read(timeout, cancellation);
        case ReadPath::MixedCan:
            return mixed_.read_iso15765(timeout, cancellation);
        }
        return fastecu::fail(ErrorKind::Internal, "unknown test read path");
    }

    void expect_read(std::uint16_t timeout, std::function<QByteArray(std::uint16_t)> action)
    {
        if (GetParam() == ReadPath::KlineRaw)
        {
            EXPECT_CALL(serial_.fake(), read_serial_data(::testing::_)).Times(0);
            EXPECT_CALL(serial_.fake(), read_serial_obd_data(timeout)).WillOnce(std::move(action));
        }
        else
        {
            EXPECT_CALL(serial_.fake(), read_serial_obd_data(::testing::_)).Times(0);
            EXPECT_CALL(serial_.fake(), read_serial_data(timeout)).WillOnce(std::move(action));
        }
    }

    bool has_unblock() const
    {
        return GetParam() != ReadPath::Kline && GetParam() != ReadPath::Ssm;
    }

    void unblock()
    {
        kline_flash_.request_unblock();
        can_flash_.request_unblock();
        mixed_.request_unblock();
    }

    FakeBackedSerial<> serial_;
    mutdma::FastEcuKlineTransport kline_{serial_.get()};
    FastEcuSsmTransport ssm_{serial_.get()};
    fastecu::flash::DesktopKlineFlashTransport kline_flash_{serial_.get()};
    fastecu::flash::DesktopCanFlashTransport can_flash_{serial_.get()};
    fastecu::flash::DesktopMixedCanFlashTransport mixed_{serial_.get()};
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
        expect_read(test.expected, [](std::uint16_t) { return QByteArray("\x00\x80\xff", 3); });
        const auto result = read(test.timeout, cancellation_);
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value());
        EXPECT_EQ(result->value(), (bytes::Bytes{0x00, 0x80, 0xff}));
        ::testing::Mock::VerifyAndClearExpectations(&serial_.fake());
    }
}

TEST_P(TransportReads, SilenceIsAnEmptyOptional)
{
    expect_read(10, [](std::uint16_t) { return QByteArray{}; });
    const auto result = read(10ms, cancellation_);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST_P(TransportReads, CancellationPrecedesDisconnectAndSkipsDriver)
{
    cancellation_.set_cancelled(true);
    EXPECT_CALL(serial_.fake(), is_serial_port_open()).Times(0);
    EXPECT_CALL(serial_.fake(), read_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(serial_.fake(), read_serial_obd_data(::testing::_)).Times(0);
    const auto result = read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_P(TransportReads, DisconnectionBeforeReadSkipsDriver)
{
    EXPECT_CALL(serial_.fake(), is_serial_port_open()).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial_.fake(), read_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(serial_.fake(), read_serial_obd_data(::testing::_)).Times(0);
    const auto result = read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_P(TransportReads, DisconnectionAfterReadDiscardsBytes)
{
    EXPECT_CALL(serial_.fake(), is_serial_port_open())
        .WillOnce(::testing::Return(true))
        .WillOnce(::testing::Return(false));
    expect_read(10, [](std::uint16_t) { return QByteArray("\xaa", 1); });
    const auto result = read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST_P(TransportReads, PostReadCancellationPrecedesDisconnect)
{
    EXPECT_CALL(serial_.fake(), is_serial_port_open()).WillOnce(::testing::Return(true));
    expect_read(10,
                [this](std::uint16_t)
                {
                    cancellation_.set_cancelled(true);
                    return QByteArray("\xaa", 1);
                });
    const auto result = read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_P(TransportReads, StandardExceptionPreservesDiagnostic)
{
    expect_read(10, [](std::uint16_t) -> QByteArray { throw std::runtime_error("read failed"); });
    const auto result = read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), (fastecu::Error{ErrorKind::Internal, "read failed"}));
}

TEST_P(TransportReads, NonstandardExceptionIsContained)
{
    expect_read(10,
                [](std::uint16_t) -> QByteArray
                {
                    throw FakeBackendNonStandardFailure{}; // NOLINT(bugprone-std-exception-baseclass): catch-all probe.
                });
    const auto result = read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_EQ(result.error().detail, GetParam() == ReadPath::Ssm        ? "SSM driver read exception"
                                     : GetParam() == ReadPath::CanFlash ? "CAN driver read exception"
                                     : GetParam() == ReadPath::MixedCan ? "mixed CAN driver read exception"
                                                                        : "K-Line driver read exception");
}

TEST_P(TransportReads, CancellationPrecedesStandardException)
{
    expect_read(10,
                [this](std::uint16_t) -> QByteArray
                {
                    cancellation_.set_cancelled(true);
                    throw std::runtime_error("read failed");
                });
    const auto result = read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_P(TransportReads, CancellationPrecedesNonstandardException)
{
    expect_read(10,
                [this](std::uint16_t) -> QByteArray
                {
                    cancellation_.set_cancelled(true);
                    throw FakeBackendNonStandardFailure{}; // NOLINT(bugprone-std-exception-baseclass): catch-all probe.
                });
    const auto result = read(10ms, cancellation_);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST_P(TransportReads, UnblockPreservesInflightBytesAndSuppressesNextRead)
{
    if (!has_unblock())
    {
        GTEST_SKIP() << "logging adapters have no unblock contract";
    }
    expect_read(10,
                [this](std::uint16_t)
                {
                    unblock();
                    return QByteArray("\xaa", 1);
                });
    const auto inflight = read(10ms, cancellation_);
    ASSERT_TRUE(inflight.has_value());
    ASSERT_TRUE(inflight->has_value());
    EXPECT_EQ(inflight->value(), bytes::Bytes{0xaa});
    ::testing::Mock::VerifyAndClearExpectations(&serial_.fake());
    EXPECT_CALL(serial_.fake(), read_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(serial_.fake(), read_serial_obd_data(::testing::_)).Times(0);
    const auto subsequent = read(10ms, cancellation_);
    ASSERT_FALSE(subsequent.has_value());
    EXPECT_EQ(subsequent.error().kind, ErrorKind::Cancelled);
}

TEST(TransportReadPrechecks, ClosedAdaptersKeepCancellationAndModePrecedence)
{
    FakeCancellationToken cancellation;
    fastecu::flash::DesktopKlineFlashTransport kline{nullptr};
    fastecu::flash::DesktopCanFlashTransport can{nullptr};
    fastecu::flash::DesktopMixedCanFlashTransport mixed{nullptr};
    const auto raw = kline.read_raw(10ms, cancellation);
    ASSERT_FALSE(raw.has_value());
    EXPECT_EQ(raw.error(), (fastecu::Error{ErrorKind::Disconnected, "read_raw() called after close()"}));
    const auto can_closed = can.read(10ms, cancellation);
    ASSERT_FALSE(can_closed.has_value());
    EXPECT_EQ(can_closed.error(), (fastecu::Error{ErrorKind::Disconnected, "read() called after close()"}));

    cancellation.set_cancelled(true);
    EXPECT_EQ(kline.read_raw(10ms, cancellation).error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(can.read(10ms, cancellation).error().kind, ErrorKind::Cancelled);
    // Mixed CAN checks its lifecycle/mode before the serial-read precheck.
    const auto mixed_closed = mixed.read_iso15765(10ms, cancellation);
    ASSERT_FALSE(mixed_closed.has_value());
    EXPECT_EQ(mixed_closed.error(), (fastecu::Error{ErrorKind::Disconnected, "ISO-15765 read called after close()"}));
}

INSTANTIATE_TEST_SUITE_P(Adapters, TransportReads,
                         ::testing::Values(ReadPath::Kline, ReadPath::Ssm, ReadPath::KlineFlash, ReadPath::KlineRaw,
                                           ReadPath::CanFlash, ReadPath::MixedCan));

const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
} // namespace
