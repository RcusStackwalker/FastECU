#include "src/backend/checksum/flash_device_lookup.h"
#include <gtest/gtest.h>

using fastecu::checksum::find_flash_device;

TEST(FindFlashDevice, ReturnsDeviceForEveryMcuStringInShippedProtocolsCfg)
{
    // Every distinct <mcu> value in resources/shared/config/protocols.cfg as
    // of this writing (grep -oP '(?<=<mcu>)[^<]*' resources/shared/config/protocols.cfg
    // | sort -u), except M32170, which is not registered in flashdevices[]
    // -- exercised separately below since checksum_correction's "Unknown MCU
    // type" path is not hypothetical, it fires for that real, currently
    // shipped protocol.
    static constexpr const char *kKnown[] = {
        "M32R_128KB",
        "M32R_256KB",
        "M32R_384KB",
        "M32R_384KB_1block",
        "M32R_512KB",
        "M32R_512KB_1block",
        "M32R_512KB_4blocks",
        "M3775x",
        "M3779x",
        "MC68HC16Y5",
        "MC68HC16Y5_TPU",
        "MH8104",
        "MH8111",
        "N83M_1_5MB",
        "N83M_4MB",
        "SH7055",
        "SH7058",
        "SH7058_1block",
        "SH7058d",
        "SH7059d",
        "SH72531",
        "SH72543d",
        "SH72543R",
    };
    for (const char *name : kKnown)
    {
        EXPECT_NE(find_flash_device(name), nullptr) << name;
    }
}

TEST(FindFlashDevice, ReturnsNullForUnknownMcuType)
{
    // "M32170" is sub_ecu_mitsu_m32r_can's real, currently shipped <mcu>
    // value in protocols.cfg; it is not registered in flashdevices[].
    EXPECT_EQ(find_flash_device("M32170"), nullptr);
    EXPECT_EQ(find_flash_device("does_not_exist"), nullptr);
}

TEST(FindFlashDevice, ExposesRomsizeForSizeValidation)
{
    const auto *device = find_flash_device("M32R_512KB");
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->romsize, 512u * 1024u);
}
