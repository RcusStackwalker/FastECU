#include "src/backend/flash/ecu/beef_can_session.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/flash/ecu/denso_beef_can_profile.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

namespace fastecu::flash
{
namespace
{

using namespace std::chrono_literals;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::_;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Optional;
using ::testing::Pair;

constexpr DensoBeefCanProfile kBare{"ECU", "petrol", BeefWrongReply::kBare, BeefPduFormat::kHex};
constexpr DensoBeefCanProfile kNotValid{"ECU", "diesel", BeefWrongReply::kNotValidAnswer, BeefPduFormat::kDescribe};
constexpr DensoBeefCanProfile kDetail{"TCU", "TCU", BeefWrongReply::kErrorDetail, BeefPduFormat::kHex};

class BeefCanSessionTest : public ::testing::Test
{
  protected:
    BeefCanSession Session(const DensoBeefCanProfile& profile)
    {
        return BeefCanSession(transport, clock, cancellation, events, channel, profile);
    }

    ScriptedCanFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    uds::ScriptedUdsChannel channel;
};

TEST_F(BeefCanSessionTest, NonfatalQueryTimeoutLogsNoValidResponse)
{
    channel.ExpectSend(bytes::Bytes{0xAA});
    channel.QueueError(ErrorKind::kTimeout);

    const auto reply = Session(kBare).NonfatalQuery(bytes::Bytes{0xAA});

    EXPECT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Eq(std::nullopt));
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU")));
}

TEST_F(BeefCanSessionTest, NonfatalQueryNoFrameLogsNoValidResponse)
{
    channel.ExpectSend(bytes::Bytes{0xAA});
    channel.QueueNoFrame();

    const auto reply = Session(kBare).NonfatalQuery(bytes::Bytes{0xAA});

    EXPECT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Eq(std::nullopt));
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU")));
}

struct WrongReplyCase
{
    const char *name;
    DensoBeefCanProfile profile;
    const char *expected;
};

class BeefCanSessionWrongReplyTest : public BeefCanSessionTest, public ::testing::WithParamInterface<WrongReplyCase>
{
};

TEST_P(BeefCanSessionWrongReplyTest, NonfatalQueryBadReplyLogsPerProfile)
{
    channel.ExpectSend(bytes::Bytes{0xAA});
    channel.QueueError(ErrorKind::kBadResponse, "boom");

    const auto reply = Session(GetParam().profile).NonfatalQuery(bytes::Bytes{0xAA});

    EXPECT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Eq(std::nullopt));
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kError, GetParam().expected)));
}

INSTANTIATE_TEST_SUITE_P(Profiles, BeefCanSessionWrongReplyTest,
                         ::testing::Values(WrongReplyCase{"Bare", kBare, "Wrong response from ECU"},
                                           WrongReplyCase{"NotValid", kNotValid,
                                                          "Wrong response from ECU: Not a valid answer"},
                                           WrongReplyCase{"Detail", kDetail, "Wrong response from TCU: boom"}),
                         [](const auto& info) { return std::string(info.param.name); });

TEST_F(BeefCanSessionTest, NonfatalQueryPassesCancelAndDisconnectThrough)
{
    for (const ErrorKind kind : {ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        channel.ExpectSend(bytes::Bytes{0xAA});
        channel.QueueError(kind);

        EXPECT_THAT(Session(kBare).NonfatalQuery(bytes::Bytes{0xAA}), IsErrWith(kind, _));
    }
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, NonfatalQueryReturnsTheFrame)
{
    channel.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel.QueueReceive(bytes::Bytes{0x50, 0x03});

    const auto reply = Session(kBare).NonfatalQuery(bytes::Bytes{0x10, 0x03});

    ASSERT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Optional(Eq(bytes::Bytes{0x50, 0x03})));
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, NonfatalQueryUsesTheReadTimeoutAndPostSendDelay)
{
    channel.ExpectSend(bytes::Bytes{0xAA});
    channel.QueueNoFrame();

    ASSERT_THAT(Session(kBare).NonfatalQuery(bytes::Bytes{0xAA}), IsOk());

    EXPECT_EQ(channel.last_timeout, 2000ms);
    EXPECT_EQ(clock.Elapsed(), 50ms);
}

bytes::Bytes BeefReply(bytes::Byte opcode, bytes::ByteView payload)
{
    bytes::Bytes frame{0xBE, 0xEF, 0x00, static_cast<bytes::Byte>(payload.size() + 1), opcode};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

TEST_F(BeefCanSessionTest, ExchangeWrapsAndUnwrapsBeef)
{
    channel.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel.QueueReceive(BeefReply(0x45, bytes::Bytes{0x00, 0x00, 0x10, 0x00}));

    const auto payload = Session(kBare).Exchange(0x05, {}, 4, 500ms);

    ASSERT_THAT(payload, IsOk());
    EXPECT_THAT(*payload, Eq(bytes::Bytes{0x00, 0x00, 0x10, 0x00}));
    EXPECT_EQ(channel.last_timeout, 500ms);
}

TEST_F(BeefCanSessionTest, ExchangeRejectsAWrongOpcode)
{
    channel.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel.QueueReceive(BeefReply(0x46, {}));

    EXPECT_THAT(Session(kBare).Exchange(0x05, {}, 0, 500ms),
                IsErrWith(ErrorKind::kBadResponse, "unexpected BEEF response opcode 0x46, expected 0x45"));
}

TEST_F(BeefCanSessionTest, ExchangeRejectsAShortPayload)
{
    channel.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel.QueueReceive(BeefReply(0x45, bytes::Bytes{0x01}));

    EXPECT_THAT(Session(kBare).Exchange(0x05, {}, 4, 500ms),
                IsErrWith(ErrorKind::kBadResponse, "short BEEF response payload"));
}

TEST_F(BeefCanSessionTest, ExchangeTimeoutIsTheReturnedErrorAndLogsNothing)
{
    channel.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel.QueueNoFrame();

    EXPECT_THAT(Session(kNotValid).Exchange(0x05, {}, 0, 500ms),
                IsErrWith(ErrorKind::kTimeout, "no CAN response within the read timeout"));
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, ExchangeSurfacesAChannelBadResponse)
{
    channel.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel.QueueError(ErrorKind::kBadResponse, "expected CAN reply id 0x7e8, got 0x123");

    EXPECT_THAT(Session(kNotValid).Exchange(0x05, {}, 0, 500ms),
                IsErrWith(ErrorKind::kBadResponse, "expected CAN reply id 0x7e8, got 0x123"));
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, RequestSessionAcceptsThePositiveReply)
{
    channel.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel.QueueReceive(bytes::Bytes{0x50, 0x03});

    EXPECT_THAT(Session(kBare).RequestSession(0x03), IsOkAnd(true));
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, RequestSessionStepsOverSilence)
{
    channel.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel.QueueNoFrame();

    EXPECT_THAT(Session(kBare).RequestSession(0x03), IsOkAnd(false));
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU")));
}

TEST_F(BeefCanSessionTest, WrongRepliesRenderInTheProfilePduFormat)
{
    const bytes::Bytes negative{0x7F, 0x10, 0x22};
    channel.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel.QueueReceive(negative);
    EXPECT_THAT(Session(kBare).RequestSession(0x03), IsOkAnd(false));

    channel.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel.QueueReceive(negative);
    EXPECT_THAT(Session(kNotValid).RequestSession(0x03), IsOkAnd(false));

    EXPECT_THAT(events.logs,
                ElementsAre(Pair(LogLevel::kError, "Wrong response from ECU: 7f 10 22 "),
                            Pair(LogLevel::kError, "Wrong response from ECU: " + uds::Describe(negative))));
}

TEST_F(BeefCanSessionTest, RequestEcuIdParsesFiveBytesAndNamesAReadImage)
{
    const bytes::Bytes reply{0xEA, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::optional<std::string> rom_id;
    channel.ExpectSend(bytes::Bytes{0xAA});
    channel.QueueReceive(reply);
    channel.ExpectSend(bytes::Bytes{0xAA});
    channel.QueueReceive(reply);

    ASSERT_THAT(Session(kBare).RequestEcuId(false, rom_id), IsOk());
    EXPECT_THAT(rom_id, Eq(std::nullopt));
    ASSERT_THAT(Session(kBare).RequestEcuId(true, rom_id), IsOk());
    EXPECT_THAT(rom_id, Optional(Eq("0102030405_")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "ECU ID: 0102030405")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Requesting ECU ID")));
}

TEST_F(BeefCanSessionTest, RequestEcuIdStepsOverAShortReply)
{
    std::optional<std::string> rom_id;
    channel.ExpectSend(bytes::Bytes{0xAA});
    channel.QueueReceive(bytes::Bytes{0xEA, 0x01});

    ASSERT_THAT(Session(kBare).RequestEcuId(true, rom_id), IsOk());
    EXPECT_THAT(rom_id, Eq(std::nullopt));
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kInfo, "Requesting ECU ID"),
                                         Pair(LogLevel::kError, "Wrong response from ECU: ea 01 ")));
}

TEST_F(BeefCanSessionTest, RequestCvnLogsUppercaseCompactHex)
{
    channel.ExpectSend(bytes::Bytes{0x09, 0x06});
    channel.QueueReceive(bytes::Bytes{0x49, 0x06, 0x01, 0xAB, 0xCD});

    ASSERT_THAT(Session(kBare).RequestCvn(), IsOk());
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kInfo, "Requesting CVN"), Pair(LogLevel::kInfo, "CVN: ABCD")));
}

TEST_F(BeefCanSessionTest, RequestCvnLogsAWrongReply)
{
    channel.ExpectSend(bytes::Bytes{0x09, 0x06});
    channel.QueueReceive(bytes::Bytes{0x7F, 0x09});

    ASSERT_THAT(Session(kDetail).RequestCvn(), IsOk());
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kInfo, "Requesting CVN"),
                                         Pair(LogLevel::kError, "Wrong response from TCU: 7f 09 ")));
}

TEST_F(BeefCanSessionTest, IdentityStringReturnsTheTail)
{
    channel.ExpectSend(bytes::Bytes{0x09, 0x02});
    channel.QueueReceive(bytes::Bytes{0x49, 0x02, 0x01, 'V', '1'});

    EXPECT_THAT(Session(kBare).IdentityString(bytes::Bytes{0x09, 0x02}, 0x49, 0x02, "VIN"),
                IsOkAnd(Optional(Eq("V1"))));
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kInfo, "VIN: V1")));
}

TEST_F(BeefCanSessionTest, IdentityStringStepsOverAMismatch)
{
    channel.ExpectSend(bytes::Bytes{0x09, 0x02});
    channel.QueueReceive(bytes::Bytes{0x49, 0x04, 0x01, 'V'});

    EXPECT_THAT(Session(kBare).IdentityString(bytes::Bytes{0x09, 0x02}, 0x49, 0x02, "VIN"), IsOkAnd(Eq(std::nullopt)));
    EXPECT_THAT(events.logs, ElementsAre(Pair(LogLevel::kError, "Wrong response from ECU: 49 04 01 56 ")));
}

} // namespace
} // namespace fastecu::flash
