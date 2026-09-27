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
bytes::Bytes b(std::initializer_list<int> values)
{
    bytes::Bytes out;
    for (int v : values)
    {
        out.push_back(static_cast<bytes::Byte>(v));
    }
    return out;
}

// Appends the 8-bit sum of all bytes as a checksum.
bytes::Bytes with_checksum(bytes::Bytes frame)
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
const bytes::Bytes kRomRaiderEcuInit = b({
    0x80, 0xF0, 0x10, 0x39, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x73, 0xFA, 0xCB,
    0x84, 0x2B, 0x83, 0xFE, 0xA8, 0x00, 0x00, 0x00, 0x60, 0xCE, 0xD4, 0xFD, 0xB0, 0x60, 0x00, 0x0F,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDC, 0x00, 0x00, 0x55, 0x1E, 0x30, 0xC0, 0xF2, 0x22, 0x00,
    0x00, 0x40, 0xFB, 0x00, 0xE1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x59,
});

// The shortest frame that still carries an ECU ID: 13 bytes plus checksum.
const bytes::Bytes kShortEcuInit =
    b({0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x6C});

const bytes::Bytes kShortTcuInit =
    b({0x80, 0xF0, 0x18, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x74});

struct Harness
{
    FakeDiagnosticLink link;
    FakeClock clock;
    FakeCancellationToken token;

    fastecu::Result<SsmIdentity> run(SsmVariant variant, SsmTarget target = SsmTarget::Ecu)
    {
        return identify_ssm_ecu(link, clock, token, SsmIdentifyRequest{variant, target});
    }
};
} // namespace

TEST(SsmFrame, AddsHeaderLengthAndChecksum)
{
    EXPECT_EQ(ssm_frame(b({0xBF}), SsmTarget::Ecu), b({0x80, 0x10, 0xF0, 0x01, 0xBF, 0x40}));
    EXPECT_EQ(ssm_frame(b({0xBF}), SsmTarget::Tcu), b({0x80, 0x18, 0xF0, 0x01, 0xBF, 0x48}));
    EXPECT_EQ(ssm_frame(b({0xA8, 0x00, 0x00, 0x00, 0x08}), SsmTarget::Ecu),
              b({0x80, 0x10, 0xF0, 0x05, 0xA8, 0x00, 0x00, 0x00, 0x08, 0x35}));
}

TEST(ParseSsmEcuId, ReadsFiveBytesAtOffsetEight)
{
    EXPECT_EQ(parse_ssm_ecu_id(kRomRaiderEcuInit), std::optional<std::string>("3152584006"));
    EXPECT_EQ(parse_ssm_ecu_id(kShortEcuInit), std::optional<std::string>("3152584006"));
}

TEST(ParseSsmEcuId, RejectsFramesTooShortForAnId)
{
    const bytes::Bytes twelve(kShortEcuInit.begin(), kShortEcuInit.begin() + 12);
    const bytes::Bytes thirteen(kShortEcuInit.begin(), kShortEcuInit.begin() + 13);
    EXPECT_EQ(parse_ssm_ecu_id(twelve), std::nullopt);
    EXPECT_EQ(parse_ssm_ecu_id(thirteen), std::optional<std::string>("3152584006"));
}

namespace
{
const std::string kKlineOpen = "open kline header=None iso14230=false baud=4800 start=00 tester=00 target=00";
}

TEST(IdentifyKlineSsm2, SendsTheInitRequestAndReturnsTheEcuId)
{
    Harness h;
    h.link.queue_read(kRomRaiderEcuInit);
    const auto result = h.run(SsmVariant::KlineSsm2);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "3152584006");
    EXPECT_EQ(result->init_response, kRomRaiderEcuInit);
    EXPECT_THAT(h.link.calls, ElementsAre(kKlineOpen, "write 80 10 F0 01 BF 40", "read 200"));
    EXPECT_EQ(h.clock.elapsed(), 200ms);
}

TEST(IdentifyKlineSsm2, TcuIsAddressedAsEighteen)
{
    Harness h;
    h.link.queue_read(kShortTcuInit);
    ASSERT_THAT(h.run(SsmVariant::KlineSsm2, SsmTarget::Tcu), IsOk());
    EXPECT_EQ(h.link.calls.at(1), "write 80 18 F0 01 BF 48");
}

TEST(IdentifyKlineSsm2, AssemblesTheFrameFromPartialReads)
{
    Harness h;
    h.link.queue_read(b({0x80, 0xF0}));
    h.link.queue_read(b({0x10, 0x09, 0xFF}));
    h.link.queue_read(b({0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06}));
    h.link.queue_read(b({0x6C}));
    const auto result = h.run(SsmVariant::KlineSsm2);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->init_response, kShortEcuInit);
    EXPECT_THAT(h.link.calls,
                ElementsAre(kKlineOpen, "write 80 10 F0 01 BF 40", "read 200", "read 50", "read 50", "read 50"));
}

TEST(IdentifyKlineSsm2, NoAnswerIsTimeoutAfterTheLegacyReadBudget)
{
    Harness h;
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErr(ErrorKind::Timeout));
    // open, write, one 200 ms read, then ten 50 ms reads waiting for a header.
    EXPECT_EQ(h.link.calls.size(), 13U);
    EXPECT_EQ(h.link.calls.back(), "read 50");
}

TEST(IdentifyKlineSsm2, IncompleteHeaderIsBadResponse)
{
    Harness h;
    h.link.queue_read(b({0x80, 0xF0}));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("header incomplete")));
}

TEST(IdentifyKlineSsm2, TruncatedBodyIsBadResponse)
{
    Harness h;
    h.link.queue_read(b({0x80, 0xF0, 0x10, 0x09, 0xFF}));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("truncated")));
    // The header was already complete, so all ten 50 ms reads wait for the body.
    EXPECT_EQ(h.link.calls.size(), 13U);
}

TEST(IdentifyKlineSsm2, KlineTrailingBytesAreDropped)
{
    Harness h;
    bytes::Bytes with_tail = kShortEcuInit;
    with_tail.push_back(0xAA);
    with_tail.push_back(0xBB);
    h.link.queue_read(with_tail);
    const auto result = h.run(SsmVariant::KlineSsm2);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->init_response, kShortEcuInit);
}

TEST(IdentifyKlineSsm2, RejectsAZeroLengthFrame)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x80, 0xF0, 0x10, 0x00})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("no response code")));
}

TEST(IdentifyKlineSsm2, RejectsAWrongHeaderByte)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x81, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("header byte")));
}

TEST(IdentifyKlineSsm2, RejectsAWrongTesterId)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x80, 0xF1, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("tester id")));
}

TEST(IdentifyKlineSsm2, KlineRejectsAnswerFromTheOtherUnit)
{
    Harness h;
    h.link.queue_read(kShortTcuInit); // the ECU was asked
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2, SsmTarget::Ecu),
                IsErrWith(ErrorKind::BadResponse, HasSubstr("target id")));
}

TEST(IdentifyKlineSsm2, RejectsAWrongResponseCode)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x80, 0xF0, 0x10, 0x09, 0xE8, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("response code is E8")));
}

TEST(IdentifyKlineSsm2, RejectsABadChecksum)
{
    Harness h;
    bytes::Bytes corrupt = kShortEcuInit;
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    corrupt.back() ^= 0x01;
    h.link.queue_read(corrupt);
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("checksum")));
}

TEST(IdentifyKlineSsm2, AValidFrameTooShortForAnIdIsBadResponse)
{
    Harness h;
    h.link.queue_read(with_checksum(b({0x80, 0xF0, 0x10, 0x02, 0xFF, 0x11})));
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErrWith(ErrorKind::BadResponse, HasSubstr("ECU ID")));
}

TEST(IdentifyKlineSsm2, CancelledSleepIsCancelled)
{
    Harness h;
    h.token.set_cancelled(true);
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErr(ErrorKind::Cancelled));
    EXPECT_THAT(h.link.calls, ElementsAre(kKlineOpen, "write 80 10 F0 01 BF 40"));
}

TEST(IdentifyKlineSsm2, LinkErrorsPassThrough)
{
    Harness h;
    h.link.queue_read_error(ErrorKind::Disconnected);
    EXPECT_THAT(h.run(SsmVariant::KlineSsm2), IsErr(ErrorKind::Disconnected));

    Harness opening;
    opening.link.queue_open(fastecu::fail(ErrorKind::Disconnected, "no port"));
    EXPECT_THAT(opening.run(SsmVariant::KlineSsm2), IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(opening.link.calls.size(), 1U);
}

namespace
{
const std::string kSsm1Open =
    "open kline header=None iso14230=false baud=1953 start=00 tester=00 target=00 parity=Even";

// The 12 reads that follow the two wake-up writes, all silent.
void queue_silent_wakeup(FakeDiagnosticLink& link)
{
    for (int i = 0; i < 12; ++i)
    {
        link.queue_no_frame();
    }
}

std::vector<std::string> ssm1_calls_through_first_frame()
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
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    const auto result = h.run(SsmVariant::Ssm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "3152584006");
    EXPECT_EQ(result->init_response, kShortEcuInit);
    auto expected = ssm1_calls_through_first_frame();
    expected.emplace_back("read 100"); // the trailing read comes back empty
    EXPECT_EQ(h.link.calls, expected);
}

TEST(IdentifySsm1, WakeupResponsesAreNotPartOfTheFrame)
{
    Harness h;
    h.link.queue_read(b({0x01, 0x02}));
    for (int i = 0; i < 11; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(kShortEcuInit);
    const auto result = h.run(SsmVariant::Ssm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->init_response, kShortEcuInit);
}

// Pinned: SSM1 is checked by length only. RomRaider's checks are SSM2's.
TEST(IdentifySsm1, OnlyTheLengthIsChecked)
{
    Harness h;
    queue_silent_wakeup(h.link);
    const bytes::Bytes odd = b({0x12, 0x34, 0x56, 0x09, 0x00, 0xA2, 0x10, 0x11, 0x31, 0x52, 0x58, 0x40, 0x06, 0x00});
    h.link.queue_read(odd);
    ASSERT_THAT(h.run(SsmVariant::Ssm1), IsOk());
}

TEST(IdentifySsm1, LengthMismatchIsBadResponse)
{
    Harness h;
    queue_silent_wakeup(h.link);
    bytes::Bytes mismatch = kShortEcuInit;
    mismatch[3] = 0x0A;
    h.link.queue_read(mismatch);
    EXPECT_THAT(h.run(SsmVariant::Ssm1), IsErr(ErrorKind::BadResponse));
}

TEST(IdentifySsm1, ShortFrameIsBadResponseNotAnOutOfRangeRead)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(b({0x80, 0xF0}));
    EXPECT_THAT(h.run(SsmVariant::Ssm1), IsErr(ErrorKind::BadResponse));
}

TEST(IdentifySsm1, NoAnswerIsTimeout)
{
    Harness h;
    EXPECT_THAT(h.run(SsmVariant::Ssm1), IsErr(ErrorKind::Timeout));
    EXPECT_EQ(h.link.calls, ssm1_calls_through_first_frame());
}

TEST(IdentifySsm1, ATrailingFrameWithAnIdReplacesTheFirst)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    const bytes::Bytes second = b({0x80, 0xF0, 0x10, 0x09, 0xFF, 0xA2, 0x10, 0x11, 0x01, 0x02, 0x03, 0x04, 0x05, 0x5A});
    h.link.queue_read(second);
    const auto result = h.run(SsmVariant::Ssm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "0102030405");
    EXPECT_EQ(result->init_response, second);
    auto expected = ssm1_calls_through_first_frame();
    expected.emplace_back("read 100"); // the second frame
    expected.emplace_back("read 100"); // the drain, empty
    EXPECT_EQ(h.link.calls, expected);
}

TEST(IdentifySsm1, ATrailingFrameTooShortForAnIdIsIgnored)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    h.link.queue_read(b({0x01, 0x02}));
    const auto result = h.run(SsmVariant::Ssm1);
    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->ecu_id, "3152584006");
    EXPECT_EQ(result->init_response, kShortEcuInit);
}

TEST(IdentifySsm1, Ssm1DrainStopsAfterOneHundredReads)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    for (int i = 0; i < 150; ++i)
    {
        h.link.queue_read(b({0x55}));
    }
    ASSERT_THAT(h.run(SsmVariant::Ssm1), IsOk());
    const auto trailing = std::count(h.link.calls.begin(), h.link.calls.end(), std::string("read 100"));
    EXPECT_EQ(trailing, 101); // one trailing frame, then at most 100 drain reads
}

TEST(IdentifySsm1, CancellingDuringTheDrainIsCancelled)
{
    Harness h;
    queue_silent_wakeup(h.link);
    h.link.queue_read(kShortEcuInit);
    for (int i = 0; i < 150; ++i)
    {
        h.link.queue_read(b({0x55}));
    }
    // 18 calls reach the first trailing read; cancel a few reads into the drain.
    h.token.set_predicate([&h] { return h.link.calls.size() > 22; });
    EXPECT_THAT(h.run(SsmVariant::Ssm1), IsErr(ErrorKind::Cancelled));
}

TEST(IdentifyIso15765Uds, ReadsF182FromTheEcu)
{
    Harness h;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x12, 0x34, 0x56, 0x78, 0x9A}));
    const auto result = h.run(SsmVariant::Iso15765Uds);
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
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82, 0x01}));
    ASSERT_THAT(h.run(SsmVariant::Iso15765Uds, SsmTarget::Tcu), IsOk());
    EXPECT_EQ(h.link.calls.at(0), "open can iso15765=true bitrate=500000 extended=false source=7E1 destination=7E8");
    EXPECT_EQ(h.link.calls.at(1), "write 00 00 07 E1 22 F1 82");
}

TEST(IdentifyIso15765Uds, NegativeResponseIsBadResponse)
{
    Harness h;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x7F, 0x22, 0x31}));
    EXPECT_THAT(h.run(SsmVariant::Iso15765Uds), IsErr(ErrorKind::BadResponse));
}

TEST(IdentifyIso15765Uds, AnAnswerWithNoIdBytesIsBadResponse)
{
    Harness h;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x62, 0xF1, 0x82}));
    EXPECT_THAT(h.run(SsmVariant::Iso15765Uds), IsErr(ErrorKind::BadResponse));
}

TEST(IdentifyIso15765Uds, NoAnswerIsTimeout)
{
    Harness h;
    EXPECT_THAT(h.run(SsmVariant::Iso15765Uds), IsErr(ErrorKind::Timeout));
}
