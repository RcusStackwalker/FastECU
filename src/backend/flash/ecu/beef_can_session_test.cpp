#include "src/backend/flash/ecu/beef_can_session.h"

#include <chrono>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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
using ::testing::_;
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

} // namespace
} // namespace fastecu::flash
