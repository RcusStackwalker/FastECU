#include "src/backend/flash/flash_device_lookup.h"

#include <cstdint>
#include <gtest/gtest.h>

using fastecu::flash::find_flash_device;
using fastecu::flash::find_flash_device_index;

TEST(FindFlashDevice, ReturnsDeviceForEveryMcuStringInShippedProtocolsCfg)
{
    // Every distinct <mcu> value in resources/shared/config/protocols.cfg as
    // of this writing (grep -oP '(?<=<mcu>)[^<]*' resources/shared/config/protocols.cfg
    // | sort -u), except M32170, which is not registered in flashdevices[]
    // -- exercised separately below since checksum correction's "Unknown MCU
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
        EXPECT_GE(find_flash_device_index(name), 0) << name;
    }
}

TEST(FindFlashDevice, ReturnsNullForUnknownMcuType)
{
    // "M32170" is sub_ecu_mitsu_m32r_can's real, currently shipped <mcu>
    // value in protocols.cfg; it is not registered in flashdevices[].
    EXPECT_EQ(find_flash_device("M32170"), nullptr);
    EXPECT_EQ(find_flash_device("does_not_exist"), nullptr);
    EXPECT_EQ(find_flash_device_index("M32170"), -1);
    EXPECT_EQ(find_flash_device_index("UNKNOWN_MCU"), -1);
}

TEST(FindFlashDevice, ExposesRomsizeForSizeValidation)
{
    const auto *device = find_flash_device("M32R_512KB");
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->romsize, 512u * 1024u);
}

TEST(FindFlashDevice, IndexAndPointerAgree)
{
    const int index = find_flash_device_index("M32R_384KB_1block");
    const flashdev_t *device = find_flash_device("M32R_384KB_1block");

    ASSERT_GE(index, 0);
    ASSERT_NE(device, nullptr);
    EXPECT_STREQ(flashdevices[index].name, "M32R_384KB_1block");
    EXPECT_STREQ(device->name, "M32R_384KB_1block");
    EXPECT_EQ(device->romsize, flashdevices[index].romsize);
    EXPECT_EQ(device->fblocks[0].start, flashdevices[index].fblocks[0].start);
}

TEST(FlashDeviceTable, MatchesExpectedSummariesAndNamedAnomalies)
{
    struct FlashDeviceSummary
    {
        const char *name;
        std::uint32_t romsize;
        unsigned numblocks;
        std::uint32_t firstBlockStart;
        std::uint32_t finalBlockEnd;
    };

    static constexpr FlashDeviceSummary kExpected[] = {
        {"M32R_128KB", 0x20000, 2, 0x0, 0x20000},
        {"M32R_256KB", 0x40000, 7, 0x0, 0x40000},
        {"M32R_384KB", 0x60000, 9, 0x0, 0x60000},
        {"M32R_512KB", 0x80000, 11, 0x0, 0x80000},
        {"M32R_512KB_1block", 0x80000, 1, 0x0, 0x80000},
        {"M32R_512KB_4blocks", 0x80000, 4, 0x0, 0x80000},
        {"M32R_384KB_1block", 0x60000, 1, 0x8000, 0x60000},
        {"MC68HC16Y5", 0x28000, 10, 0x0, 0x30000},
        {"MC68HC16Y5_TPU", 0x1000, 4, 0x60000, 0x64000},
        {"SH7051", 0x40000, 1, 0x0, 0x40000},
        {"SH7055", 0x80000, 16, 0x0, 0x80000},
        {"SH7058", 0x100000, 16, 0x0, 0x100000},
        {"SH7058_1block", 0x100000, 1, 0x0, 0x100000},
        {"SH7058d", 0x100000, 16, 0x0, 0x100000},
        {"SH7059d", 0x180000, 16, 0x0, 0x180000},
        {"SH72543d", 0x200000, 1, 0x8000, 0x1fff00},
        {"SH72531", 0x140000, 3, 0x0, 0x138000},
        {"N83M_4MB", 0x3e4000, 3, 0x08f9c000, 0x09380000},
        {"N83M_1_5MB", 0x174000, 3, 0x08f9c000, 0x09120000},
        {"SH72543R", 0x200000, 2, 0x0, 0x200000},
        {"MH8104", 0x80000, 4, 0x0, 0x80000},
        {"MH5006", 0x100000, 4, 0x0, 0x100000},
        {"MH8111", 0x180000, 4, 0x0, 0x180000},
        {"M3779x", 0x10000, 1, 0x8000, 0x17fff},
        {"M3775x", 0x10000, 1, 0x1000, 0x10fff},
    };
    constexpr std::size_t kCount = sizeof(kExpected) / sizeof(kExpected[0]);

    for (std::size_t deviceIndex = 0; deviceIndex < kCount; ++deviceIndex)
    {
        const flashdev_t& actual = flashdevices[deviceIndex];
        const FlashDeviceSummary& summary = kExpected[deviceIndex];
        EXPECT_STREQ(actual.name, summary.name);
        EXPECT_EQ(actual.romsize, summary.romsize);
        EXPECT_EQ(actual.numblocks, summary.numblocks);
        EXPECT_EQ(actual.fblocks[0].start, summary.firstBlockStart);
        const flashblock& finalBlock = actual.fblocks[actual.numblocks - 1];
        EXPECT_EQ(finalBlock.start + finalBlock.len, summary.finalBlockEnd);
        for (unsigned blockIndex = 0; blockIndex < actual.numblocks; ++blockIndex)
        {
            EXPECT_GT(actual.fblocks[blockIndex].len, 0u);
            if (blockIndex > 0)
            {
                EXPECT_LE(actual.fblocks[blockIndex - 1].start, actual.fblocks[blockIndex].start);
            }
        }
    }

    const flashdev_t& sentinel = flashdevices[kCount];
    EXPECT_EQ(sentinel.name, nullptr);
    EXPECT_EQ(sentinel.romsize, std::uint32_t(0));
    EXPECT_EQ(sentinel.numblocks, 0u);
    EXPECT_EQ(sentinel.fblocks, nullptr);
    EXPECT_EQ(sentinel.rblocks, nullptr);
    EXPECT_EQ(sentinel.kblocks, nullptr);
    EXPECT_EQ(sentinel.eblocks, nullptr);

    const flashdev_t *sh72531 = find_flash_device("SH72531");
    const flashdev_t *mc68 = find_flash_device("MC68HC16Y5");
    const flashdev_t *n83 = find_flash_device("N83M_1_5MB");
    const flashdev_t *tpu = find_flash_device("MC68HC16Y5_TPU");
    ASSERT_NE(sh72531, nullptr);
    ASSERT_NE(mc68, nullptr);
    ASSERT_NE(n83, nullptr);
    ASSERT_NE(tpu, nullptr);

    // These checks freeze the named quirks in the current table; they do
    // not declare the anomalous ranges correct.

    // SH72531 block 2 starts 0x8000 bytes before block 1 ends.
    EXPECT_EQ(sh72531->fblocks[1].start + sh72531->fblocks[1].len - sh72531->fblocks[2].start, std::uint32_t(0x8000));

    // MC68HC16Y5 leaves 0x8000 between block 7 and block 8 and its final two
    // blocks extend past the declared 0x28000 ROM size.
    EXPECT_EQ(mc68->fblocks[8].start - (mc68->fblocks[7].start + mc68->fblocks[7].len), std::uint32_t(0x8000));
    EXPECT_EQ(mc68->fblocks[9].start + mc68->fblocks[9].len, std::uint32_t(0x30000));

    // N83M_1_5MB block coverage extends 0x10000 past base + declared ROM size.
    EXPECT_EQ(n83->fblocks[2].start + n83->fblocks[2].len - (n83->fblocks[0].start + n83->romsize),
              std::uint32_t(0x10000));

    // MC68HC16Y5_TPU declares 0x1000 ROM bytes but exposes four contiguous
    // 0x1000 blocks; three blocks extend beyond base + romsize.
    EXPECT_EQ(tpu->fblocks[3].start + tpu->fblocks[3].len - (tpu->fblocks[0].start + tpu->romsize),
              std::uint32_t(0x3000));
}
