#include "src/backend/flash/ecu/beef_can_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/flash/ecu/denso_beef_can_profile.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_types.h"
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

    const auto reply = Session(kBare).NonfatalQuery(bytes::Bytes{0xAA});

    ASSERT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Eq(std::nullopt));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU")));
}

TEST_F(BeefCanSessionTest, NonfatalQueryNoFrameLogsNoValidResponse)
{
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueNoFrame();

    const auto reply = Session(kBare).NonfatalQuery(bytes::Bytes{0xAA});

    ASSERT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Eq(std::nullopt));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU")));
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
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueError(ErrorKind::kBadResponse, "boom");

    const auto reply = Session(GetParam().profile).NonfatalQuery(bytes::Bytes{0xAA});

    ASSERT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Eq(std::nullopt));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, GetParam().expected)));
}

INSTANTIATE_TEST_SUITE_P(Profiles, BeefCanSessionWrongReplyTest,
                         ::testing::Values(WrongReplyCase{"Bare", kBare, "Wrong response from ECU"},
                                           WrongReplyCase{"NotValid", kNotValid,
                                                          "Wrong response from ECU: Not a valid answer"},
                                           WrongReplyCase{"Detail", kDetail, "Wrong response from TCU: boom"}),
                         [](const auto& test_param) { return std::string(test_param.param.name); });

TEST_F(BeefCanSessionTest, NonfatalQueryPassesCancelAndDisconnectThrough)
{
    for (const ErrorKind kind : {ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(static_cast<int>(kind));
        channel_.ExpectSend(bytes::Bytes{0xAA});
        channel_.QueueError(kind);

        EXPECT_THAT(Session(kBare).NonfatalQuery(bytes::Bytes{0xAA}), IsErrWith(kind, _));
    }
    EXPECT_THAT(events_.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, NonfatalQueryReturnsTheFrame)
{
    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueReceive(bytes::Bytes{0x50, 0x03});

    const auto reply = Session(kBare).NonfatalQuery(bytes::Bytes{0x10, 0x03});

    ASSERT_THAT(reply, IsOk());
    EXPECT_THAT(*reply, Optional(Eq(bytes::Bytes{0x50, 0x03})));
    EXPECT_THAT(events_.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, NonfatalQueryUsesTheReadTimeoutAndPostSendDelay)
{
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueNoFrame();

    ASSERT_THAT(Session(kBare).NonfatalQuery(bytes::Bytes{0xAA}), IsOk());

    EXPECT_EQ(channel_.last_timeout, 2000ms);
    EXPECT_EQ(clock_.Elapsed(), 50ms);
}

bytes::Bytes BeefReply(bytes::Byte opcode, bytes::ByteView payload)
{
    bytes::Bytes frame{0xBE, 0xEF, 0x00, static_cast<bytes::Byte>(payload.size() + 1), opcode};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

TEST_F(BeefCanSessionTest, ExchangeWrapsAndUnwrapsBeef)
{
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel_.QueueReceive(BeefReply(0x45, bytes::Bytes{0x00, 0x00, 0x10, 0x00}));

    const auto payload = Session(kBare).Exchange(0x05, {}, 4, 500ms);

    ASSERT_THAT(payload, IsOk());
    EXPECT_THAT(*payload, Eq(bytes::Bytes{0x00, 0x00, 0x10, 0x00}));
    EXPECT_EQ(channel_.last_timeout, 500ms);
}

TEST_F(BeefCanSessionTest, ExchangeRejectsAWrongOpcode)
{
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel_.QueueReceive(BeefReply(0x46, {}));

    EXPECT_THAT(Session(kBare).Exchange(0x05, {}, 0, 500ms),
                IsErrWith(ErrorKind::kBadResponse, "unexpected BEEF response opcode 0x46, expected 0x45"));
}

TEST_F(BeefCanSessionTest, ExchangeRejectsAShortPayload)
{
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel_.QueueReceive(BeefReply(0x45, bytes::Bytes{0x01}));

    EXPECT_THAT(Session(kBare).Exchange(0x05, {}, 4, 500ms),
                IsErrWith(ErrorKind::kBadResponse, "short BEEF response payload"));
}

TEST_F(BeefCanSessionTest, ExchangeTimeoutIsTheReturnedErrorAndLogsNothing)
{
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel_.QueueNoFrame();

    EXPECT_THAT(Session(kNotValid).Exchange(0x05, {}, 0, 500ms),
                IsErrWith(ErrorKind::kTimeout, "no CAN response within the read timeout"));
    EXPECT_THAT(events_.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, ExchangeSurfacesAChannelBadResponse)
{
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
    channel_.QueueError(ErrorKind::kBadResponse, "expected CAN reply id 0x7e8, got 0x123");

    EXPECT_THAT(Session(kNotValid).Exchange(0x05, {}, 0, 500ms),
                IsErrWith(ErrorKind::kBadResponse, "expected CAN reply id 0x7e8, got 0x123"));
    EXPECT_THAT(events_.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, RequestSessionAcceptsThePositiveReply)
{
    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueReceive(bytes::Bytes{0x50, 0x03});

    EXPECT_THAT(Session(kBare).RequestSession(0x03), IsOkAnd(true));
    EXPECT_THAT(events_.logs, IsEmpty());
}

TEST_F(BeefCanSessionTest, RequestSessionStepsOverSilence)
{
    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueNoFrame();

    EXPECT_THAT(Session(kBare).RequestSession(0x03), IsOkAnd(false));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "No valid response from ECU")));
}

TEST_F(BeefCanSessionTest, WrongRepliesRenderInTheProfilePduFormat)
{
    const bytes::Bytes negative{0x7F, 0x10, 0x22};
    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueReceive(negative);
    EXPECT_THAT(Session(kBare).RequestSession(0x03), IsOkAnd(false));

    channel_.ExpectSend(bytes::Bytes{0x10, 0x03});
    channel_.QueueReceive(negative);
    EXPECT_THAT(Session(kNotValid).RequestSession(0x03), IsOkAnd(false));

    EXPECT_THAT(events_.logs,
                ElementsAre(Pair(LogLevel::kError, "Wrong response from ECU: 7f 10 22 "),
                            Pair(LogLevel::kError, "Wrong response from ECU: " + uds::Describe(negative))));
}

TEST_F(BeefCanSessionTest, RequestEcuIdParsesFiveBytesAndNamesAReadImage)
{
    const bytes::Bytes reply{0xEA, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::optional<std::string> rom_id;
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueReceive(reply);
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueReceive(reply);

    ASSERT_THAT(Session(kBare).RequestEcuId(false, rom_id), IsOk());
    EXPECT_THAT(rom_id, Eq(std::nullopt));
    ASSERT_THAT(Session(kBare).RequestEcuId(true, rom_id), IsOk());
    EXPECT_THAT(rom_id, Optional(Eq("0102030405_")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "ECU ID: 0102030405")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "Requesting ECU ID")));
}

TEST_F(BeefCanSessionTest, RequestEcuIdStepsOverAShortReply)
{
    std::optional<std::string> rom_id;
    channel_.ExpectSend(bytes::Bytes{0xAA});
    channel_.QueueReceive(bytes::Bytes{0xEA, 0x01});

    ASSERT_THAT(Session(kBare).RequestEcuId(true, rom_id), IsOk());
    EXPECT_THAT(rom_id, Eq(std::nullopt));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kInfo, "Requesting ECU ID"),
                                          Pair(LogLevel::kError, "Wrong response from ECU: ea 01 ")));
}

TEST_F(BeefCanSessionTest, RequestCvnLogsUppercaseCompactHex)
{
    channel_.ExpectSend(bytes::Bytes{0x09, 0x06});
    channel_.QueueReceive(bytes::Bytes{0x49, 0x06, 0x01, 0xAB, 0xCD});

    ASSERT_THAT(Session(kBare).RequestCvn(), IsOk());
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kInfo, "Requesting CVN"), Pair(LogLevel::kInfo, "CVN: ABCD")));
}

TEST_F(BeefCanSessionTest, RequestCvnLogsAWrongReply)
{
    channel_.ExpectSend(bytes::Bytes{0x09, 0x06});
    channel_.QueueReceive(bytes::Bytes{0x7F, 0x09});

    ASSERT_THAT(Session(kDetail).RequestCvn(), IsOk());
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kInfo, "Requesting CVN"),
                                          Pair(LogLevel::kError, "Wrong response from TCU: 7f 09 ")));
}

TEST_F(BeefCanSessionTest, IdentityStringReturnsTheTail)
{
    channel_.ExpectSend(bytes::Bytes{0x09, 0x02});
    channel_.QueueReceive(bytes::Bytes{0x49, 0x02, 0x01, 'V', '1'});

    EXPECT_THAT(Session(kBare).IdentityString(bytes::Bytes{0x09, 0x02}, 0x49, 0x02, "VIN"),
                IsOkAnd(Optional(Eq("V1"))));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kInfo, "VIN: V1")));
}

TEST_F(BeefCanSessionTest, IdentityStringStepsOverAMismatch)
{
    channel_.ExpectSend(bytes::Bytes{0x09, 0x02});
    channel_.QueueReceive(bytes::Bytes{0x49, 0x04, 0x01, 'V'});

    EXPECT_THAT(Session(kBare).IdentityString(bytes::Bytes{0x09, 0x02}, 0x49, 0x02, "VIN"), IsOkAnd(Eq(std::nullopt)));
    EXPECT_THAT(events_.logs, ElementsAre(Pair(LogLevel::kError, "Wrong response from ECU: 49 04 01 56 ")));
}

bytes::Bytes Be32(std::uint32_t value)
{
    return {static_cast<bytes::Byte>(value >> 24U), static_cast<bytes::Byte>(value >> 16U),
            static_cast<bytes::Byte>(value >> 8U), static_cast<bytes::Byte>(value)};
}

bytes::Bytes BeefCall(bytes::Byte opcode, bytes::ByteView payload = {})
{
    bytes::Bytes frame{0xBE, 0xEF, 0x00, static_cast<bytes::Byte>(payload.size() + 1), opcode};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// Literal SH7058 geometry and the CRC of an all-zero block, as in the petrol
// executor suite; no production table or CRC code builds these.
struct BlockFixture
{
    MemoryRegion region;
    std::uint32_t zero_crc;
};

constexpr std::array<BlockFixture, 16> kBlocks{{
    {{0x00000000, 0x00001000}, 0xF722EF49},
    {{0x00001000, 0x00001000}, 0xF722EF49},
    {{0x00002000, 0x00001000}, 0xF722EF49},
    {{0x00003000, 0x00001000}, 0xF722EF49},
    {{0x00004000, 0x00001000}, 0xF722EF49},
    {{0x00005000, 0x00001000}, 0xF722EF49},
    {{0x00006000, 0x00001000}, 0xF722EF49},
    {{0x00007000, 0x00001000}, 0xF722EF49},
    {{0x00008000, 0x00018000}, 0xC85F0DED},
    {{0x00020000, 0x00020000}, 0xC39BE4A8},
    {{0x00040000, 0x00020000}, 0xC39BE4A8},
    {{0x00060000, 0x00020000}, 0xC39BE4A8},
    {{0x00080000, 0x00020000}, 0xC39BE4A8},
    {{0x000A0000, 0x00020000}, 0xC39BE4A8},
    {{0x000C0000, 0x00020000}, 0xC39BE4A8},
    {{0x000E0000, 0x00020000}, 0xC39BE4A8},
}};

constexpr std::uint32_t kRomSize = 0x00100000;

class BeefCanSessionFlashTest : public BeefCanSessionTest
{
  protected:
    Result<FlashPlan> PlanFor(FlashOperation operation)
    {
        return BuildSubaruDensoSh7058CanPlan(
            operation, "sub_ecu_denso_sh7058_can", "SH7058", bytes::Bytes(kRomSize, bytes::Byte{0}),
            KernelImage{.id = "petrol-sh7058-kernel", .load_address = 0xFFFF3000, .bytes = {0x01, 0x02, 0x03, 0x04}});
    }

    // One CRC query: the request, the four-byte reply, then the stale-frame
    // drain read.
    void ScriptCrc(const MemoryRegion& block, std::uint32_t crc)
    {
        bytes::Bytes payload = Be32(block.start);
        payload.push_back(0x00);
        payload.push_back(static_cast<bytes::Byte>(block.length >> 16U));
        payload.push_back(static_cast<bytes::Byte>(block.length >> 8U));
        payload.push_back(static_cast<bytes::Byte>(block.length));
        channel_.ExpectSend(BeefCall(0x02, payload));
        channel_.QueueReceive(BeefReply(0x42, Be32(crc)));
        transport_.QueueNoFrame();
    }

    void ScriptCompare(std::optional<std::size_t> differing = std::nullopt)
    {
        for (std::size_t index = 0; index < kBlocks.size(); ++index)
        {
            ScriptCrc(kBlocks[index].region, index == differing ? 0xDEADBEEF : kBlocks[index].zero_crc);
        }
    }

    void ScriptInitialize(bytes::Byte mode_opcode)
    {
        channel_.ExpectSend(BeefCall(0x05));
        channel_.QueueReceive(BeefReply(0x45, bytes::Bytes{0x00, 0x00, 0x10, 0x00}));
        channel_.ExpectSend(BeefCall(0x06));
        channel_.QueueReceive(BeefReply(0x46, bytes::Bytes{0x00, 0x00, 0x02, 0x00}));
        channel_.ExpectSend(BeefCall(mode_opcode));
        channel_.QueueReceive(BeefReply(static_cast<bytes::Byte>(mode_opcode | 0x40), {}));
    }
};

TEST_F(BeefCanSessionFlashTest, QueryCrcReadsFourBytesAndDrainsTheStaleFrame)
{
    ScriptCrc(kBlocks[0].region, 0xF722EF49);

    EXPECT_THAT(Session(kBare).QueryCrc(kBlocks[0].region), IsOkAnd(Eq(0xF722EF49U)));
    EXPECT_EQ(channel_.last_timeout, 3000ms);
    EXPECT_THAT(transport_.ReadTimeouts(), ElementsAre(200ms));
}

class BeefCanSessionFamilyLabelTest : public BeefCanSessionFlashTest,
                                      public ::testing::WithParamInterface<const DensoBeefCanProfile *>
{
};

TEST_P(BeefCanSessionFamilyLabelTest, CompareBlocksNamesTheFamilyWhenABlockLeavesTheImage)
{
    const auto plan = PlanFor(FlashOperation::kWrite);
    ASSERT_THAT(plan, IsOk());

    EXPECT_THAT(
        Session(*GetParam()).CompareBlocks(*plan, bytes::ByteView{}, nullptr),
        IsErrWith(ErrorKind::kInvalidConfig, std::format("{} CRC block is outside the image", GetParam()->family)));
}

INSTANTIATE_TEST_SUITE_P(Profiles, BeefCanSessionFamilyLabelTest, ::testing::Values(&kBare, &kNotValid, &kDetail),
                         [](const auto& test_param) { return std::string(test_param.param->family); });

TEST_F(BeefCanSessionFlashTest, CompareBlocksFlagsOnlyDifferingBlocks)
{
    const auto plan = PlanFor(FlashOperation::kWrite);
    ASSERT_THAT(plan, IsOk());
    ScriptCompare(3);

    const auto result = Session(kBare).CompareBlocks(*plan, plan->ImageOrEmpty(), nullptr);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->changed_count, 1U);
    EXPECT_TRUE(result->modified[3]);
    EXPECT_EQ(std::count(result->modified.begin(), result->modified.end(), true), 1);
}

TEST_F(BeefCanSessionFlashTest, InitializeFlashSelectsTestOrRealWriteMode)
{
    ScriptInitialize(0x21);
    ScriptInitialize(0x20);

    ASSERT_THAT(Session(kBare).InitializeFlash(true), IsOk());
    ASSERT_THAT(Session(kBare).InitializeFlash(false), IsOk());

    EXPECT_THAT(events_.logs,
                Contains(Pair(LogLevel::kInfo, "Test write mode on, no actual flash write is performed")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "Test write mode off, perform actual flash write")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, ": 0x1000")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, ": 0x0200")));
    EXPECT_EQ(channel_.timeouts.front(), 500ms);
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanSessionFlashTest, WriteMemoryWithNoDifferenceFlashesNothing)
{
    const auto plan = PlanFor(FlashOperation::kWrite);
    ASSERT_THAT(plan, IsOk());
    ScriptCompare();
    PhaseSequence phases(events_, 4);
    PhaseReporter compare = phases.Start("Compare", static_cast<int>(kBlocks.size()));

    ASSERT_THAT(Session(kBare).WriteMemory(*plan, phases, compare), IsOk());

    EXPECT_THAT(events_.logs,
                Contains(Pair(LogLevel::kInfo,
                              "*** Compare results no difference between ROM and ECU data, no flashing needed! ***")));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanSessionFlashTest, WriteMemoryFailsWithTheExchangeErrorWhenTheVoltageCheckTimesOut)
{
    const auto plan = PlanFor(FlashOperation::kWrite);
    ASSERT_THAT(plan, IsOk());
    ScriptCompare(0);
    ScriptInitialize(0x20);
    channel_.ExpectSend(BeefCall(0x04));
    channel_.QueueNoFrame();
    PhaseSequence phases(events_, 4);
    PhaseReporter compare = phases.Start("Compare", static_cast<int>(kBlocks.size()));

    EXPECT_THAT(Session(kBare).WriteMemory(*plan, phases, compare),
                IsErrWith(ErrorKind::kTimeout, "no CAN response within the read timeout"));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "Block 0 reflash failed.")));
}

// The family suites script no channel_-level bad reply and no out-of-image
// compare, so the wording a profile selects for those is pinned here.
TEST(DensoBeefCanProfileTest, FamilyProfilesKeepTheirLogWording)
{
    const auto expect_profile = [](const DensoBeefCanProfile& profile, std::string_view device, std::string_view family,
                                   BeefWrongReply wrong_reply, BeefPduFormat pdu_format)
    {
        EXPECT_EQ(profile.device, device);
        EXPECT_EQ(profile.family, family);
        EXPECT_EQ(profile.wrong_reply, wrong_reply);
        EXPECT_EQ(profile.pdu_format, pdu_format);
    };

    expect_profile(kSubaruDensoSh7058CanBeefProfile, "ECU", "petrol", BeefWrongReply::kBare, BeefPduFormat::kHex);
    expect_profile(kSubaruDensoSh7058CanDieselBeefProfile, "ECU", "diesel", BeefWrongReply::kNotValidAnswer,
                   BeefPduFormat::kDescribe);
}

} // namespace
} // namespace fastecu::flash
