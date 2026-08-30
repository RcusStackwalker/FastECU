#include "src/platform/desktop/common/logging/cdbg_serial_setup.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"

namespace fastecu::desktop::logging
{
namespace
{

class RawCanSetupRecording
{
  public:
    RawCanSetupActions actions()
    {
        return {
            .set_iso14230 =
                [this](bool enabled)
            {
                calls_.push_back(std::format("iso14230:{}", enabled));
                return true;
            },
            .set_iso14230_header =
                [this](bool enabled)
            {
                calls_.push_back(std::format("iso14230-header:{}", enabled));
                return true;
            },
            .set_raw_can =
                [this](bool enabled)
            {
                calls_.push_back(std::format("raw-can:{}", enabled));
                return true;
            },
            .set_iso15765 =
                [this](bool enabled)
            {
                calls_.push_back(std::format("iso15765:{}", enabled));
                return true;
            },
            .set_identifier_width =
                [this](dashboard::CanIdentifierWidth width)
            {
                calls_.push_back(std::format("identifier-width:{}", static_cast<unsigned>(width)));
                return true;
            },
            .set_bitrate =
                [this](std::uint32_t bitrate)
            {
                calls_.push_back(std::format("bitrate:{}", bitrate));
                return true;
            },
            .set_reply_id =
                [this](std::uint32_t id)
            {
                calls_.push_back(std::format("reply-id:{:#x}", id));
                return true;
            },
        };
    }

    const std::vector<std::string>& calls() const
    {
        return calls_;
    }

  private:
    std::vector<std::string> calls_;
};

RawCanSetupProfile standard_profile()
{
    return {
        .bitrate = 500000,
        .identifier_width = dashboard::CanIdentifierWidth::Standard,
        .reply_id = MitsuColtCanCdbg::kReplyCanId,
    };
}

void expect_invalid_config_for(const fastecu::Status& result, std::string_view operation)
{
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_NE(result.error().detail.find(operation), std::string::npos);
}

TEST(CdbgSerialSetupTest, AppliesProfileValuesInRawCanSetupOrder)
{
    RawCanSetupRecording recording;
    const RawCanSetupProfile profile{
        .bitrate = 250000,
        .identifier_width = dashboard::CanIdentifierWidth::Extended,
        .reply_id = 0x18daf110,
    };

    EXPECT_TRUE(configure_raw_can(profile, recording.actions()).has_value());
    EXPECT_EQ(recording.calls(), (std::vector<std::string>{
                                     "iso14230:false",
                                     "iso14230-header:false",
                                     "raw-can:true",
                                     "iso15765:false",
                                     "identifier-width:29",
                                     "bitrate:250000",
                                     "reply-id:0x18daf110",
                                 }));
}

TEST(CdbgSerialSetupTest, AppliesColtDefaults)
{
    RawCanSetupRecording recording;
    const RawCanSetupProfile profile{
        .bitrate = 500000,
        .identifier_width = dashboard::CanIdentifierWidth::Standard,
        .reply_id = MitsuColtCanCdbg::kReplyCanId,
    };

    ASSERT_TRUE(configure_raw_can(profile, recording.actions()).has_value());
    EXPECT_EQ(recording.calls(), (std::vector<std::string>{
                                     "iso14230:false",
                                     "iso14230-header:false",
                                     "raw-can:true",
                                     "iso15765:false",
                                     "identifier-width:11",
                                     "bitrate:500000",
                                     "reply-id:0x631",
                                 }));
}

TEST(CdbgSerialSetupTest, RejectsIso14230Failure)
{
    RawCanSetupRecording recording;
    auto actions = recording.actions();
    actions.set_iso14230 = [](bool) { return false; };

    expect_invalid_config_for(configure_raw_can(standard_profile(), actions), "ISO 14230 mode");
}

TEST(CdbgSerialSetupTest, RejectsIso14230HeaderFailure)
{
    RawCanSetupRecording recording;
    auto actions = recording.actions();
    actions.set_iso14230_header = [](bool) { return false; };

    expect_invalid_config_for(configure_raw_can(standard_profile(), actions), "ISO 14230 header");
}

TEST(CdbgSerialSetupTest, RejectsRawCanFailure)
{
    RawCanSetupRecording recording;
    auto actions = recording.actions();
    actions.set_raw_can = [](bool) { return false; };

    expect_invalid_config_for(configure_raw_can(standard_profile(), actions), "raw CAN mode");
}

TEST(CdbgSerialSetupTest, RejectsIso15765Failure)
{
    RawCanSetupRecording recording;
    auto actions = recording.actions();
    actions.set_iso15765 = [](bool) { return false; };

    expect_invalid_config_for(configure_raw_can(standard_profile(), actions), "ISO 15765 mode");
}

TEST(CdbgSerialSetupTest, RejectsIdentifierWidthFailure)
{
    RawCanSetupRecording recording;
    auto actions = recording.actions();
    actions.set_identifier_width = [](dashboard::CanIdentifierWidth) { return false; };

    expect_invalid_config_for(configure_raw_can(standard_profile(), actions), "CAN identifier width");
}

TEST(CdbgSerialSetupTest, RejectsBitrateFailure)
{
    RawCanSetupRecording recording;
    auto actions = recording.actions();
    actions.set_bitrate = [](std::uint32_t) { return false; };

    expect_invalid_config_for(configure_raw_can(standard_profile(), actions), "CAN bitrate");
}

TEST(CdbgSerialSetupTest, RejectsReplyIdFailure)
{
    RawCanSetupRecording recording;
    auto actions = recording.actions();
    actions.set_reply_id = [](std::uint32_t) { return false; };

    expect_invalid_config_for(configure_raw_can(standard_profile(), actions), "CAN reply identifier");
}

TEST(CdbgSerialSetupTest, RejectsMissingCallback)
{
    RawCanSetupRecording recording;
    auto actions = recording.actions();
    actions.set_reply_id = {};

    expect_invalid_config_for(configure_raw_can(standard_profile(), actions), "CAN reply identifier");
}

} // namespace
} // namespace fastecu::desktop::logging
