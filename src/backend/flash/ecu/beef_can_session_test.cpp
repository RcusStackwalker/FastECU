#include "src/backend/flash/ecu/beef_can_session.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/ecu/denso_beef_can_profile.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_types.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_types.h"
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

constexpr DensoBeefCanProfile kHexEcu{.device = "ECU", .family = "petrol", .pdu_format = BeefPduFormat::kHex};
constexpr DensoBeefCanProfile kDescribeEcu{.device = "ECU", .family = "diesel", .pdu_format = BeefPduFormat::kDescribe};
constexpr DensoBeefCanProfile kHexTcu{.device = "TCU", .family = "TCU", .pdu_format = BeefPduFormat::kHex};

class BeefCanSessionTest : public ::testing::Test
{
  protected:
    BeefCanSession Session(const DensoBeefCanProfile& profile)
    {
        return BeefCanSession(transport_, clock_, cancellation_, events_, channel_, profile);
    }

    ScriptedCanFlashTransport transport_;
    FakeClock clock_;
    FakeCancellationToken cancellation_;
    RecordingEventSink events_;
    uds::ScriptedUdsChannel channel_;
};

TEST_F(BeefCanSessionTest, NonfatalQueryTimeoutLogsNoValidResponse)
{
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueError(ErrorKind::kTimeout);

    const auto reply = Session(kHexEcu).NonfatalQuery(bytes::Bytes{0xAA});

    ASSERT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Eq(std::nullopt));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU")));
}

TEST_F(BeefCanSessionTest, NonfatalQueryNoFrameLogsNoValidResponse)
{
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueNoFrame();

    const auto reply = Session(kHexEcu).NonfatalQuery(bytes::Bytes{0xAA});

    ASSERT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Eq(std::nullopt));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU")));
}

TEST_F(BeefCanSessionTest, NonfatalQueryBadReplyLogsTheErrorDetailUnderTheDeviceName)
{
    for (const DensoBeefCanProfile *profile : {&kHexEcu, &kHexTcu})
    {
        SCOPED_TRACE(profile->device);
        channel_.ExpectSend(bytes::Bytes{0xAA});
        channel_.QueueError(ErrorKind::kBadResponse, "boom");
        events_.logs.clear();

        const auto reply = Session(*profile).NonfatalQuery(bytes::Bytes{0xAA});

        ASSERT_THAT(reply, IsOk());
        EXPECT_THAT(*reply, Eq(std::nullopt));
        EXPECT_THAT(events_.logs,
                    ElementsAre(Pair(LogLevel::kError, std::format("Wrong response from {}: boom", profile->device))));
    }
}

TEST_F(BeefCanSessionTest, LogFailureNamesTimeoutsAndBadRepliesAndStaysSilentOtherwise)
{
    Session(kHexTcu).LogFailure(Error{ErrorKind::kTimeout, "late"});
    Session(kHexTcu).LogFailure(Error{ErrorKind::kBadResponse, "garbled"});
    Session(kHexTcu).LogFailure(Error{ErrorKind::kCancelled, "stop"});
    Session(kHexTcu).LogFailure(Error{ErrorKind::kDisconnected, "gone"});

    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU"),
                                          Pair(LogLevel::kError, "Wrong response from TCU: garbled")));
}

TEST_F(BeefCanSessionTest, NonfatalQueryPassesCancelAndDisconnectThrough)
{
    for (const ErrorKind kind : {ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(static_cast<int>(kind));
        channel_.ExpectSend(bytes::Bytes{0xAA});
        channel_.QueueError(kind);

        EXPECT_THAT(Session(kHexEcu).NonfatalQuery(bytes::Bytes{0xAA}), IsErrWith(kind, _));
    }
    EXPECT_THAT(events_.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, NonfatalQueryReturnsTheFrame)
{
    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueReceive(bytes::Bytes{0x50, 0x03});

    const auto reply = Session(kHexEcu).NonfatalQuery(bytes::Bytes{0x10, 0x03});

    ASSERT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Optional(Eq(bytes::Bytes{0x50, 0x03})));
    EXPECT_THAT(events_.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, NonfatalQueryUsesTheReadTimeoutAndPostSendDelay)
{
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueNoFrame();

    ASSERT_THAT(Session(kHexEcu).NonfatalQuery(bytes::Bytes{0xAA}), IsOk());

    EXPECT_EQ(channel_.last_timeout, 2000ms);
    EXPECT_EQ(clock_.Elapsed(), 50ms);
}

TEST_F(BeefCanSessionTest, RequestSessionAcceptsThePositiveReply)
{
    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueReceive(bytes::Bytes{0x50, 0x03});

    EXPECT_THAT(Session(kHexEcu).RequestSession(0x03), IsOkAnd(true));
    EXPECT_THAT(events_.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, RequestSessionStepsOverSilence)
{
    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueNoFrame();

    EXPECT_THAT(Session(kHexEcu).RequestSession(0x03), IsOkAnd(false));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU")));
}

TEST_F(BeefCanSessionTest, WrongRepliesRenderInTheProfilePduFormat)
{
    const bytes::Bytes negative{0x7F, 0x10, 0x22};
    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueReceive(negative);
    EXPECT_THAT(Session(kHexEcu).RequestSession(0x03), IsOkAnd(false));

    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueReceive(negative);
    EXPECT_THAT(Session(kDescribeEcu).RequestSession(0x03), IsOkAnd(false));

    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "Wrong response from ECU: 7f 10 22 "),
                                          Pair(LogLevel::kError, "Wrong response from ECU: Conditions not correct")));
}

TEST_F(BeefCanSessionTest, RequestEcuIdParsesFiveBytesAndNamesAReadImage)
{
    const bytes::Bytes reply{0xEA, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::optional<std::string> rom_id;
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueReceive(reply);
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueReceive(reply);

    ASSERT_THAT(Session(kHexEcu).RequestEcuId(false, rom_id), IsOk());
    EXPECT_THAT(rom_id, Eq(std::nullopt));
    ASSERT_THAT(Session(kHexEcu).RequestEcuId(true, rom_id), IsOk());
    EXPECT_THAT(rom_id, Optional(Eq("0102030405_")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "ECU ID: 0102030405")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "Requesting ECU ID")));
}

TEST_F(BeefCanSessionTest, RequestEcuIdStepsOverAShortReply)
{
    std::optional<std::string> rom_id;
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueReceive(bytes::Bytes{0xEA, 0x01});

    ASSERT_THAT(Session(kHexEcu).RequestEcuId(true, rom_id), IsOk());
    EXPECT_THAT(rom_id, Eq(std::nullopt));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kInfo, "Requesting ECU ID"),
                                          Pair(LogLevel::kError, "Wrong response from ECU: ea 01 ")));
}

TEST_F(BeefCanSessionTest, RequestCvnLogsUppercaseCompactHex)
{
    channel_.ExpectSend(bytes::Bytes{0x09, 0x06});
    channel_.QueueReceive(bytes::Bytes{0x49, 0x06, 0x01, 0xAB, 0xCD});

    ASSERT_THAT(Session(kHexEcu).RequestCvn(), IsOk());
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kInfo, "Requesting CVN"), Pair(LogLevel::kInfo, "CVN: ABCD")));
}

TEST_F(BeefCanSessionTest, RequestCvnLogsAWrongReply)
{
    channel_.ExpectSend(bytes::Bytes{0x09, 0x06});
    channel_.QueueReceive(bytes::Bytes{0x7F, 0x09});

    ASSERT_THAT(Session(kHexTcu).RequestCvn(), IsOk());
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kInfo, "Requesting CVN"),
                                          Pair(LogLevel::kError, "Wrong response from TCU: 7f 09 ")));
}

TEST_F(BeefCanSessionTest, IdentityStringReturnsTheTail)
{
    channel_.ExpectSend(bytes::Bytes{0x09, 0x02});
    channel_.QueueReceive(bytes::Bytes{0x49, 0x02, 0x01, 'V', '1'});

    EXPECT_THAT(Session(kHexEcu).IdentityString(bytes::Bytes{0x09, 0x02}, 0x49, 0x02, "VIN"),
                IsOkAnd(Optional(Eq("V1"))));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kInfo, "VIN: V1")));
}

TEST_F(BeefCanSessionTest, IdentityStringStepsOverAMismatch)
{
    channel_.ExpectSend(bytes::Bytes{0x09, 0x02});
    channel_.QueueReceive(bytes::Bytes{0x49, 0x04, 0x01, 'V'});

    EXPECT_THAT(Session(kHexEcu).IdentityString(bytes::Bytes{0x09, 0x02}, 0x49, 0x02, "VIN"),
                IsOkAnd(Eq(std::nullopt)));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "Wrong response from ECU: 49 04 01 56 ")));
}

// Family wording and formatting inputs remain pure profile data. Behavior is
// exercised through the public kernel operations in beef_can_kernel_test.cpp.
TEST(DensoBeefCanProfileTest, FamilyProfilesKeepTheirLogWording)
{
    const auto expect_profile = [](const DensoBeefCanProfile& profile, std::string_view device, std::string_view family,
                                   BeefPduFormat pdu_format, std::chrono::milliseconds init_timeout,
                                   bool upper_hex_sizes)
    {
        SCOPED_TRACE(family);
        EXPECT_EQ(profile.device, device);
        EXPECT_EQ(profile.family, family);
        EXPECT_EQ(profile.pdu_format, pdu_format);
        EXPECT_EQ(profile.init_timeout, init_timeout);
        EXPECT_EQ(profile.upper_hex_sizes, upper_hex_sizes);
    };

    expect_profile(kSubaruDensoSh7058CanBeefProfile, "ECU", "petrol", BeefPduFormat::kHex, 500ms, false);
    expect_profile(kSubaruDensoSh7058CanDieselBeefProfile, "ECU", "diesel", BeefPduFormat::kDescribe, 500ms, false);
    expect_profile(kSubaruTcuDensoSh705xCanBeefProfile, "TCU", "TCU", BeefPduFormat::kHex, 2000ms, true);
}

} // namespace
} // namespace fastecu::flash
