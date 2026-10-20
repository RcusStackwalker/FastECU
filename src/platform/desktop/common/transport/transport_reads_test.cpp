#include <array>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/serial/testing/fake_backed_serial.h"
#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_kline_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h"
#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"
#include "src/platform/desktop/common/transport/serial_read.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::ICancellationToken;
using ReadResult = fastecu::Result<std::optional<bytes::Bytes>>;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::StrEq;

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
        ASSERT_THAT(mixed_.Configure({}), IsOk());
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
        ASSERT_THAT(result, IsOk());
        ASSERT_TRUE(result->has_value());
        EXPECT_EQ(result->value(), (bytes::Bytes{0x00, 0x80, 0xff}));
        ::testing::Mock::VerifyAndClearExpectations(&serial_.Fake());
    }
}

TEST_P(TransportReads, SilenceIsAnEmptyOptional)
{
    ExpectRead(10, [](std::uint16_t) { return QByteArray{}; });
    const auto result = Read(10ms, cancellation_);
    ASSERT_THAT(result, IsOk());
    EXPECT_FALSE(result->has_value());
}

TEST_P(TransportReads, CancellationPrecedesDisconnectAndSkipsDriver)
{
    cancellation_.SetCancelled(true);
    EXPECT_CALL(serial_.Fake(), IsSerialPortOpen()).Times(0);
    EXPECT_CALL(serial_.Fake(), ReadSerialData(::testing::_)).Times(0);
    EXPECT_CALL(serial_.Fake(), ReadSerialObdData(::testing::_)).Times(0);
    const auto result = Read(10ms, cancellation_);
    EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
}

TEST_P(TransportReads, DisconnectionBeforeReadSkipsDriver)
{
    EXPECT_CALL(serial_.Fake(), IsSerialPortOpen()).WillOnce(::testing::Return(false));
    EXPECT_CALL(serial_.Fake(), ReadSerialData(::testing::_)).Times(0);
    EXPECT_CALL(serial_.Fake(), ReadSerialObdData(::testing::_)).Times(0);
    const auto result = Read(10ms, cancellation_);
    EXPECT_THAT(result, IsErr(ErrorKind::kDisconnected));
}

TEST_P(TransportReads, DisconnectionAfterReadDiscardsBytes)
{
    EXPECT_CALL(serial_.Fake(), IsSerialPortOpen())
        .WillOnce(::testing::Return(true))
        .WillOnce(::testing::Return(false));
    ExpectRead(10, [](std::uint16_t) { return QByteArray("\xaa", 1); });
    const auto result = Read(10ms, cancellation_);
    EXPECT_THAT(result, IsErr(ErrorKind::kDisconnected));
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
    EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
}

TEST_P(TransportReads, StandardExceptionPreservesDiagnostic)
{
    ExpectRead(10, [](std::uint16_t) -> QByteArray { throw std::runtime_error("read failed"); });
    const auto result = Read(10ms, cancellation_);
    EXPECT_THAT(result, IsErrWith(ErrorKind::kInternal, StrEq("read failed")));
}

TEST_P(TransportReads, NonstandardExceptionIsContained)
{
    ExpectRead(10,
               [](std::uint16_t) -> QByteArray
               {
                   throw FakeBackendNonStandardFailure{}; // NOLINT(bugprone-std-exception-baseclass): catch-all probe.
               });
    const auto result = Read(10ms, cancellation_);
    EXPECT_THAT(result, IsErrWith(ErrorKind::kInternal, StrEq("serial driver read exception")));
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
    EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
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
    EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
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
    ASSERT_THAT(inflight, IsOk());
    ASSERT_TRUE(inflight->has_value());
    EXPECT_EQ(inflight->value(), bytes::Bytes{0xaa});
    ::testing::Mock::VerifyAndClearExpectations(&serial_.Fake());
    EXPECT_CALL(serial_.Fake(), ReadSerialData(::testing::_)).Times(0);
    EXPECT_CALL(serial_.Fake(), ReadSerialObdData(::testing::_)).Times(0);
    const auto subsequent = Read(10ms, cancellation_);
    EXPECT_THAT(subsequent, IsErr(ErrorKind::kCancelled));
}

TEST(TransportReadPrechecks, ClosedAdaptersKeepCancellationAndModePrecedence)
{
    FakeCancellationToken cancellation;
    fastecu::flash::DesktopKlineFlashTransport kline{nullptr};
    fastecu::flash::DesktopCanFlashTransport can{nullptr};
    fastecu::flash::DesktopMixedCanFlashTransport mixed{nullptr};
    const auto raw = kline.ReadRaw(10ms, cancellation);
    EXPECT_THAT(raw, IsErrWith(ErrorKind::kDisconnected, StrEq("read_raw() called after close()")));
    const auto can_closed = can.Read(10ms, cancellation);
    EXPECT_THAT(can_closed, IsErrWith(ErrorKind::kDisconnected, StrEq("read() called after close()")));

    cancellation.SetCancelled(true);
    EXPECT_THAT(kline.ReadRaw(10ms, cancellation), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(can.Read(10ms, cancellation), fastecu::testing::IsErr(ErrorKind::kCancelled));
    // Mixed CAN checks its lifecycle/mode before the serial-read precheck.
    const auto mixed_closed = mixed.ReadIso15765(10ms, cancellation);
    EXPECT_THAT(mixed_closed, IsErrWith(ErrorKind::kDisconnected, StrEq("ISO-15765 read called after close()")));
}

TEST(GuardedDriverCall, MapsEscapingExceptionsToTheRequestedKind)
{
    using fastecu::Status;
    using fastecu::desktop::detail::GuardedDriverCall;
    EXPECT_THAT(
        GuardedDriverCall<fastecu::Result<std::size_t>>(
            ErrorKind::kInternal, "unknown",
            []() -> fastecu::Result<std::size_t>
            {
                throw FakeBackendNonStandardFailure{}; // NOLINT(bugprone-std-exception-baseclass): catch-all probe.
            }),
        IsErrWith(ErrorKind::kInternal, StrEq("unknown")));
    EXPECT_THAT(GuardedDriverCall<fastecu::Result<std::size_t>>(ErrorKind::kInternal, "unknown",
                                                                []() -> fastecu::Result<std::size_t>
                                                                { throw std::logic_error("x"); }),
                IsErrWith(ErrorKind::kInternal, StrEq("x")));
    EXPECT_THAT(GuardedDriverCall<Status>(ErrorKind::kInternal, "unknown",
                                          []() -> fastecu::Status { throw std::runtime_error("driver broke"); }),
                IsErrWith(ErrorKind::kInternal, StrEq("driver broke")));
    EXPECT_THAT(
        GuardedDriverCall<Status>(
            ErrorKind::kDisconnected, "unknown",
            []() -> fastecu::Status
            {
                throw FakeBackendNonStandardFailure{}; // NOLINT(bugprone-std-exception-baseclass): catch-all probe.
            }),
        IsErrWith(ErrorKind::kDisconnected, StrEq("unknown")));
    EXPECT_THAT(GuardedDriverCall<Status>(ErrorKind::kInternal, "unknown", []() -> fastecu::Status { return {}; }),
                IsOk());
}

TEST(GuardedDriverCall, ResetOfAClosedSerialIsDisconnected)
{
    EXPECT_THAT(fastecu::desktop::detail::ResetSerialConnection(nullptr),
                IsErrWith(ErrorKind::kDisconnected, StrEq("reset_connection() called after close()")));
}

INSTANTIATE_TEST_SUITE_P(Adapters, TransportReads,
                         ::testing::Values(ReadPath::kKline, ReadPath::kSsm, ReadPath::kKlineFlash, ReadPath::kKlineRaw,
                                           ReadPath::kCanFlash, ReadPath::kMixedCan),
                         (
                             [](const auto& info)
                             {
                                 static constexpr std::array kNames{"Kline",    "Ssm",      "KlineFlash",
                                                                    "KlineRaw", "CanFlash", "MixedCan"};
                                 return kNames[info.index];
                             }));

const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
} // namespace
