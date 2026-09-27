#include "src/backend/diagnostics/ssm_identify.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>

using namespace fastecu::diagnostics;

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
