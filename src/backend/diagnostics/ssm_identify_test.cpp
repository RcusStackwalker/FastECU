#include "src/backend/diagnostics/ssm_identify.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <algorithm>
#include <initializer_list>
#include <string>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using namespace fastecu::diagnostics;
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::FakeClock;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

namespace
{
bytes::Bytes B(std::initializer_list<int> values)
{
    bytes::Bytes out;
    for (int v : values)
    {
        out.push_back(static_cast<bytes::Byte>(v));
    }
    return out;
}

// Appends the 8-bit sum of all bytes as a checksum.
bytes::Bytes WithChecksum(bytes::Bytes frame)
{
    unsigned sum = 0;
    for (const bytes::Byte byte : frame)
    {
        sum += byte;
    }
    frame.push_back(static_cast<bytes::Byte>(sum & 0xFFU));
    return frame;
}

// RomRaider's documented ECU init response (io/protocol/ssm/iso9141/
// SSMProtocol.java, checkValidEcuInitResponse). ECU ID 3152584006.
const bytes::Bytes kRomRaiderEcuInit = B({
    0x80, 0xF0, 0x10, 0x39, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x73, 0xFA, 0xCB,
    0x84, 0x2B, 0x83, 0xFE, 0xA8, 0x00, 0x00, 0x00, 0x60, 0xCE, 0xD4, 0xFD, 0xB0, 0x60, 0x00, 0x0F,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDC, 0x00, 0x00, 0x55, 0x1E, 0x30, 0xC0, 0xF2, 0x22, 0x00,
    0x00, 0x40, 0xFB, 0x00, 0xE1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x59,
});

// The shortest frame that still carries an ECU ID: 13 bytes plus checksum.
const bytes::Bytes kShortEcuInit =
    B({0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x6C});

const bytes::Bytes kShortTcuInit =
    B({0x80, 0xF0, 0x18, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x74});

struct Harness
{
    FakeDiagnosticLink link;
    FakeClock clock;
    FakeCancellationToken token;

    fastecu::Result<SsmIdentity> Run(SsmVariant variant, SsmTarget target = SsmTarget::kEcu)
    {
        return IdentifySsmEcu(link, clock, token, SsmIdentifyRequest{variant, target});
    }
};
} // namespace

TEST(SsmFrame, AddsHeaderLengthAndChecksum)
{
    EXPECT_EQ(SsmFrame(B({0xBF}), SsmTarget::kEcu), B({0x80, 0x10, 0xF0, 0x01, 0xBF, 0x40}));
    EXPECT_EQ(SsmFrame(B({0xBF}), SsmTarget::kTcu), B({0x80, 0x18, 0xF0, 0x01, 0xBF, 0x48}));
    EXPECT_EQ(SsmFrame(B({0xA8, 0x00, 0x00, 0x00, 0x08}), SsmTarget::kEcu),
              B({0x80, 0x10, 0xF0, 0x05, 0xA8, 0x00, 0x00, 0x00, 0x08, 0x35}));
}

TEST(ParseSsmEcuId, ReadsFiveBytesAtOffsetEight)
{
    EXPECT_EQ(ParseSsmEcuId(kRomRaiderEcuInit), std::optional<std::string>("3152584006"));
    EXPECT_EQ(ParseSsmEcuId(kShortEcuInit), std::optional<std::string>("3152584006"));
}

TEST(ParseSsmEcuId, RejectsFramesTooShortForAnId)
{
    const bytes::Bytes twelve(kShortEcuInit.begin(), kShortEcuInit.begin() + 12);
    const bytes::Bytes thirteen(kShortEcuInit.begin(), kShortEcuInit.begin() + 13);
    EXPECT_EQ(ParseSsmEcuId(twelve), std::nullopt);
    EXPECT_EQ(ParseSsmEcuId(thirteen), std::optional<std::string>("3152584006"));
}

namespace
{
const std::string kKlineOpen = "open kline header=None iso14230=false baud=4800 start=00 tester=00 target=00";
}

TEST(IdentifyKlineSsm2, SendsTheInitRequestAndReturnsTheEcuId)
{
    Harness h;
    h.link.QueueRead(kRomRaiderEcuInit);
    const auto result = h.Run(SsmVariant::kKlineSsm2);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "3152584006");
    EXPECT_EQ(result->init_response, kRomRaiderEcuInit);
    EXPECT_THAT(h.link.calls, ElementsAre(kKlineOpen, "write 80 10 F0 01 BF 40", "read 200"));
    EXPECT_EQ(h.clock.Elapsed(), 200ms);
}

TEST(IdentifyKlineSsm2, TcuIsAddressedAsEighteen)
{
    Harness h;
    h.link.QueueRead(kShortTcuInit);
    ASSERT_THAT(h.Run(SsmVariant::kKlineSsm2, SsmTarget::kTcu), IsOk());
    EXPECT_EQ(h.link.calls.at(1), "write 80 18 F0 01 BF 48");
}

TEST(IdentifyKlineSsm2, AssemblesTheFrameFromPartialReads)
{
    Harness h;
    h.link.QueueRead(B({0x80, 0xF0}));
    h.link.QueueRead(B({0x10, 0x09, 0xFF}));
    h.link.QueueRead(B({0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06}));
    h.link.QueueRead(B({0x6C}));
    const auto result = h.Run(SsmVariant::kKlineSsm2);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->init_response, kShortEcuInit);
    EXPECT_THAT(h.link.calls,
                ElementsAre(kKlineOpen, "write 80 10 F0 01 BF 40", "read 200", "read 50", "read 50", "read 50"));
}

TEST(IdentifyKlineSsm2, NoAnswerIsTimeoutAfterTheLegacyReadBudget)
{
    Harness h;
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErr(ErrorKind::kTimeout));
    // open, write, one 200 ms read, then ten 50 ms reads waiting for a header.
    ASSERT_EQ(h.link.calls.size(), 13U);
    EXPECT_EQ(h.link.calls.back(), "read 50");
}

TEST(IdentifyKlineSsm2, IncompleteHeaderIsBadResponse)
{
    Harness h;
    h.link.QueueRead(B({0x80, 0xF0}));
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErrWith(ErrorKind::kBadResponse, HasSubstr("header incomplete")));
}

TEST(IdentifyKlineSsm2, TruncatedBodyIsBadResponse)
{
    Harness h;
    h.link.QueueRead(B({0x80, 0xF0, 0x10, 0x09, 0xFF}));
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErrWith(ErrorKind::kBadResponse, HasSubstr("truncated")));
    // The header was already complete, so all ten 50 ms reads wait for the body.
    EXPECT_EQ(h.link.calls.size(), 13U);
}

TEST(IdentifyKlineSsm2, KlineTrailingBytesAreDropped)
{
    Harness h;
    bytes::Bytes with_tail = kShortEcuInit;
    with_tail.push_back(0xAA);
    with_tail.push_back(0xBB);
    h.link.QueueRead(with_tail);
    const auto result = h.Run(SsmVariant::kKlineSsm2);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->init_response, kShortEcuInit);
}

TEST(IdentifyKlineSsm2, RejectsAZeroLengthFrame)
{
    Harness h;
    h.link.QueueRead(WithChecksum(B({0x80, 0xF0, 0x10, 0x00})));
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErrWith(ErrorKind::kBadResponse, HasSubstr("no response code")));
}

TEST(IdentifyKlineSsm2, RejectsAWrongHeaderByte)
{
    Harness h;
    h.link.QueueRead(WithChecksum(B({0x81, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06})));
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErrWith(ErrorKind::kBadResponse, HasSubstr("header byte")));
}

TEST(IdentifyKlineSsm2, RejectsAWrongTesterId)
{
    Harness h;
    h.link.QueueRead(WithChecksum(B({0x80, 0xF1, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06})));
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErrWith(ErrorKind::kBadResponse, HasSubstr("tester id")));
}

TEST(IdentifyKlineSsm2, KlineRejectsAnswerFromTheOtherUnit)
{
    Harness h;
    h.link.QueueRead(kShortTcuInit); // the ECU was asked
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2, SsmTarget::kEcu),
                IsErrWith(ErrorKind::kBadResponse, HasSubstr("target id")));
}

TEST(IdentifyKlineSsm2, RejectsAWrongResponseCode)
{
    Harness h;
    h.link.QueueRead(WithChecksum(B({0x80, 0xF0, 0x10, 0x09, 0xE8, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06})));
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErrWith(ErrorKind::kBadResponse, HasSubstr("response code is E8")));
}

TEST(IdentifyKlineSsm2, RejectsABadChecksum)
{
    Harness h;
    bytes::Bytes corrupt = kShortEcuInit;
    corrupt.back() ^= 0x01U;
    h.link.QueueRead(corrupt);
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErrWith(ErrorKind::kBadResponse, HasSubstr("checksum")));
}

TEST(IdentifyKlineSsm2, AValidFrameTooShortForAnIdIsBadResponse)
{
    Harness h;
    h.link.QueueRead(WithChecksum(B({0x80, 0xF0, 0x10, 0x02, 0xFF, 0x11})));
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErrWith(ErrorKind::kBadResponse, HasSubstr("ECU ID")));
}

TEST(IdentifyKlineSsm2, CancelledSleepIsCancelled)
{
    Harness h;
    h.token.SetCancelled(true);
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(h.link.calls, ElementsAre(kKlineOpen, "write 80 10 F0 01 BF 40"));
}

TEST(IdentifyKlineSsm2, LinkErrorsPassThrough)
{
    Harness h;
    h.link.QueueReadError(ErrorKind::kDisconnected);
    EXPECT_THAT(h.Run(SsmVariant::kKlineSsm2), IsErr(ErrorKind::kDisconnected));

    Harness opening;
    opening.link.QueueOpen(fastecu::Fail(ErrorKind::kDisconnected, "no port"));
    EXPECT_THAT(opening.Run(SsmVariant::kKlineSsm2), IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(opening.link.calls.size(), 1U);
}

namespace
{
const std::string kSsm1Open =
    "open kline header=None iso14230=false baud=1953 start=00 tester=00 target=00 parity=Even";

// The 12 reads that follow the two wake-up writes, all silent.
void QueueSilentWakeup(FakeDiagnosticLink& link)
{
    for (int i = 0; i < 12; ++i)
    {
        link.QueueNoFrame();
    }
}

std::vector<std::string> Ssm1CallsThroughFirstFrame()
{
    std::vector<std::string> calls{kSsm1Open, "write 78 12 34 00"};
    for (int i = 0; i < 10; ++i)
    {
        calls.emplace_back("read 500");
    }
    calls.emplace_back("write 00 46 48 49");
    calls.emplace_back("read 500");
    calls.emplace_back("read 500");
    calls.emplace_back("write 12 00 00 00");
    calls.emplace_back("read 500");
    return calls;
}
} // namespace

TEST(IdentifySsm1, SendsTheSsm1SequenceAtEvenParity)
{
    Harness h;
    QueueSilentWakeup(h.link);
    h.link.QueueRead(kShortEcuInit);
    const auto result = h.Run(SsmVariant::kSsm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "3152584006");
    EXPECT_EQ(result->init_response, kShortEcuInit);
    auto expected = Ssm1CallsThroughFirstFrame();
    expected.emplace_back("read 100"); // the trailing read comes back empty
    EXPECT_EQ(h.link.calls, expected);
}

TEST(IdentifySsm1, WakeupResponsesAreNotPartOfTheFrame)
{
    Harness h;
    h.link.QueueRead(B({0x01, 0x02}));
    for (int i = 0; i < 11; ++i)
    {
        h.link.QueueNoFrame();
    }
    h.link.QueueRead(kShortEcuInit);
    const auto result = h.Run(SsmVariant::kSsm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->init_response, kShortEcuInit);
}

// Pinned: SSM1 is checked by length only. RomRaider's checks are SSM2's.
TEST(IdentifySsm1, OnlyTheLengthIsChecked)
{
    Harness h;
    QueueSilentWakeup(h.link);
    const bytes::Bytes odd = B({0x12, 0x34, 0x56, 0x09, 0x00, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x00});
    h.link.QueueRead(odd);
    ASSERT_THAT(h.Run(SsmVariant::kSsm1), IsOk());
}

TEST(IdentifySsm1, LengthMismatchIsBadResponse)
{
    Harness h;
    QueueSilentWakeup(h.link);
    bytes::Bytes mismatch = kShortEcuInit;
    mismatch[3] = 0x0A;
    h.link.QueueRead(mismatch);
    EXPECT_THAT(h.Run(SsmVariant::kSsm1), IsErr(ErrorKind::kBadResponse));
}

TEST(IdentifySsm1, ShortFrameIsBadResponseNotAnOutOfRangeRead)
{
    Harness h;
    QueueSilentWakeup(h.link);
    h.link.QueueRead(B({0x80, 0xF0}));
    EXPECT_THAT(h.Run(SsmVariant::kSsm1), IsErr(ErrorKind::kBadResponse));
}

TEST(IdentifySsm1, NoAnswerIsTimeout)
{
    Harness h;
    EXPECT_THAT(h.Run(SsmVariant::kSsm1), IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(h.link.calls, Ssm1CallsThroughFirstFrame());
}

TEST(IdentifySsm1, ATrailingFrameWithAnIdReplacesTheFirst)
{
    Harness h;
    QueueSilentWakeup(h.link);
    h.link.QueueRead(kShortEcuInit);
    const bytes::Bytes second = B({0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x01, 0x02, 0x03, 0x04, 0x05, 0x5A});
    h.link.QueueRead(second);
    const auto result = h.Run(SsmVariant::kSsm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "0102030405");
    EXPECT_EQ(result->init_response, second);
    auto expected = Ssm1CallsThroughFirstFrame();
    expected.emplace_back("read 100"); // the second frame
    expected.emplace_back("read 100"); // the drain, empty
    EXPECT_EQ(h.link.calls, expected);
}

TEST(IdentifySsm1, ATrailingFrameTooShortForAnIdIsIgnored)
{
    Harness h;
    QueueSilentWakeup(h.link);
    h.link.QueueRead(kShortEcuInit);
    h.link.QueueRead(B({0x01, 0x02}));
    const auto result = h.Run(SsmVariant::kSsm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "3152584006");
    EXPECT_EQ(result->init_response, kShortEcuInit);
}

TEST(IdentifySsm1, Ssm1DrainStopsAfterOneHundredReads)
{
    Harness h;
    QueueSilentWakeup(h.link);
    h.link.QueueRead(kShortEcuInit);
    for (int i = 0; i < 150; ++i)
    {
        h.link.QueueRead(B({0x55}));
    }
    ASSERT_THAT(h.Run(SsmVariant::kSsm1), IsOk());
    const auto trailing = std::count(h.link.calls.begin(), h.link.calls.end(), std::string("read 100"));
    EXPECT_EQ(trailing, 101); // one trailing frame, then at most 100 drain reads
}

TEST(IdentifySsm1, CancellingDuringTheDrainIsCancelled)
{
    Harness h;
    QueueSilentWakeup(h.link);
    h.link.QueueRead(kShortEcuInit);
    for (int i = 0; i < 150; ++i)
    {
        h.link.QueueRead(B({0x55}));
    }
    // 18 calls reach the first trailing read; cancel a few reads into the drain.
    h.token.SetPredicate([&h] { return h.link.calls.size() > 22; });
    EXPECT_THAT(h.Run(SsmVariant::kSsm1), IsErr(ErrorKind::kCancelled));
}

TEST(IdentifyIso15765Uds, ReadsF182FromTheEcu)
{
    Harness h;
    h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x12, 0x34, 0x56, 0x78, 0x9A}));
    const auto result = h.Run(SsmVariant::kIso15765Uds);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "123456789A");
    EXPECT_THAT(result->init_response, IsEmpty());
    EXPECT_THAT(h.link.calls,
                ElementsAre("open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                            "write 00 00 07 E0 22 F1 82", "read 100"));
}

TEST(IdentifyIso15765Uds, TcuIsAddressedAs7E1)
{
    Harness h;
    h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x01}));
    ASSERT_THAT(h.Run(SsmVariant::kIso15765Uds, SsmTarget::kTcu), IsOk());
    EXPECT_EQ(h.link.calls.at(0), "open can iso15765=true bitrate=500000 extended=false source=7E1 destination=7E8");
    EXPECT_EQ(h.link.calls.at(1), "write 00 00 07 E1 22 F1 82");
}

TEST(IdentifyIso15765Uds, NegativeResponseIsBadResponse)
{
    Harness h;
    h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x7F, 0x22, 0x31}));
    EXPECT_THAT(h.Run(SsmVariant::kIso15765Uds), IsErr(ErrorKind::kBadResponse));
}

TEST(IdentifyIso15765Uds, AnAnswerWithNoIdBytesIsBadResponse)
{
    Harness h;
    h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82}));
    EXPECT_THAT(h.Run(SsmVariant::kIso15765Uds), IsErr(ErrorKind::kBadResponse));
}

TEST(IdentifyIso15765Uds, NoAnswerIsTimeout)
{
    Harness h;
    EXPECT_THAT(h.Run(SsmVariant::kIso15765Uds), IsErr(ErrorKind::kTimeout));
}
