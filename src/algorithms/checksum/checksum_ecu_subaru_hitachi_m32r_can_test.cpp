#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_can.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"

// The checksum layout, from the algorithm's own description:
//
//   1  32-bit sum of the words in [0x6000, 0x8000)                   at 0x7ffe8
//   2  32-bit XOR of the same words                                  at 0x7ffec
//   3  32-bit sum of the words in [0, 0x7fff0), except 0x10000       at 0x7fff0
//   4  32-bit XOR of the same words                                  at 0x7fff4
//   5  the low halves of the words in [0x4000, 0x80000), except
//      0x10000, sum to 0x5aa5, balanced by the halfword              at 0x7fffa
//   6  8-bit sum (high byte) and XOR (low byte) of every byte
//      except [0x10000, 0x10004), compared with the halfword         at 0x10000
//
// The checksums nest: slots 1 and 2 lie inside checksum 3's words, and slots
// 1 to 4 inside checksum 5's. Each expected value below is worked out by hand
// from that layout for a ROM holding a single word of 1, so a mutant that
// moves a window edge or skips a correction shows up as a wrong slot value.

namespace
{

using Rom = ChecksumEcuSubaruHitachiM32rCan;

constexpr std::size_t kChecksum1 = 0x7ffe8;
constexpr std::size_t kChecksum2 = 0x7ffec;
constexpr std::size_t kChecksum3 = 0x7fff0;
constexpr std::size_t kChecksum4 = 0x7fff4;
constexpr std::size_t kBalance = 0x7fffa;
constexpr std::size_t kChecksum6 = 0x10000;
constexpr std::uint16_t kBalanceTarget = 0x5aa5;

struct Slots
{
    std::uint32_t checksum_1;
    std::uint32_t checksum_2;
    std::uint32_t checksum_3;
    std::uint32_t checksum_4;
    std::uint16_t balance;

    bool operator==(const Slots&) const = default;
};

void PrintTo(const Slots& slots, std::ostream *os)
{
    *os << std::hex << "{1: 0x" << slots.checksum_1 << ", 2: 0x" << slots.checksum_2 << ", 3: 0x" << slots.checksum_3
        << ", 4: 0x" << slots.checksum_4 << ", balance: 0x" << slots.balance << "}";
}

Slots ReadSlots(bytes::ByteView rom)
{
    return {bytes::ReadU32Be(rom, kChecksum1), bytes::ReadU32Be(rom, kChecksum2), bytes::ReadU32Be(rom, kChecksum3),
            bytes::ReadU32Be(rom, kChecksum4), bytes::ReadU16Be(rom, kBalance)};
}

// The balance that makes checksum 5's halfwords sum to the target, given what
// the other words in its window contribute.
constexpr std::uint16_t BalanceFor(std::uint16_t window_sum)
{
    return static_cast<std::uint16_t>(kBalanceTarget - window_sum);
}

// A zero ROM with the word 0x00000001 at `address`: its slots after one pass.
struct SingleWordCase
{
    std::string_view name; // shows up in the gtest test name
    std::size_t address;
    Slots expected;
};

std::string SingleWordCaseName(const ::testing::TestParamInfo<SingleWordCase>& info)
{
    return std::string(info.param.name);
}

// Checksum 3 and 4 only: slots 3 and 4 become 1, and checksum 5 sees them.
constexpr Slots kOnlyChecksums3And4 = {0, 0, 1, 1, BalanceFor(1 + 1)};
// Checksums 3, 4 and 5: as above, plus the word itself in checksum 5's window.
constexpr Slots kChecksums3To5 = {0, 0, 1, 1, BalanceFor(1 + 1 + 1)};
// Every checksum: slots 1 and 2 become 1, slot 3 sums the word and both of
// them (3), slot 4 XORs them (1), and checksum 5 sees the word and all four.
constexpr Slots kEveryChecksum = {1, 1, 3, 1, BalanceFor(1 + 1 + 1 + 3 + 1)};

const SingleWordCase kSingleWordCases[] = {
    {"BelowChecksum5", 0x3ffc, kOnlyChecksums3And4},
    {"Checksum5Start", 0x4000, kChecksums3To5},
    {"BelowChecksum1", 0x5ffc, kChecksums3To5},
    {"Checksum1Start", 0x6000, kEveryChecksum},
    {"Checksum1End", 0x7ffc, kEveryChecksum},
    {"AboveChecksum1", 0x8000, kChecksums3To5},
    {"BelowTheExcludedWord", 0xfffc, kChecksums3To5},
    // In no window: only checksum 5's balance is written.
    {"TheExcludedWord", 0x10000, {0, 0, 0, 0, BalanceFor(0)}},
    {"AboveTheExcludedWord", 0x10004, kChecksums3To5},
    {"Checksum3End", 0x7ffe4, kChecksums3To5},
};

class ChecksumEcuSubaruHitachiM32rCanWindow : public ::testing::TestWithParam<SingleWordCase>
{
};

TEST_P(ChecksumEcuSubaruHitachiM32rCanWindow, CorrectsEachSlotForASingleWord)
{
    const SingleWordCase& c = GetParam();
    bytes::Bytes rom(Rom::kRomSize, 0x00);
    bytes::WriteU32Be(rom, c.address, 1);

    const ChecksumResult result = Rom::CalculateChecksumResult(Rom::Rom(rom));

    EXPECT_EQ(ReadSlots(result.rom_data), c.expected);
}

INSTANTIATE_TEST_SUITE_P(ChecksumPortable, ChecksumEcuSubaruHitachiM32rCanWindow, ::testing::ValuesIn(kSingleWordCases),
                         SingleWordCaseName);

// A zero ROM whose slot at `address` holds a stale 1: every slot after one pass.
struct StaleSlotCase
{
    std::string_view name; // shows up in the gtest test name
    std::size_t address;
};

std::string StaleSlotCaseName(const ::testing::TestParamInfo<StaleSlotCase>& info)
{
    return std::string(info.param.name);
}

const StaleSlotCase kStaleSlotCases[] = {
    {"Checksum1", kChecksum1},
    {"Checksum2", kChecksum2},
    // Checksum 3's words stop before its own slot, so it cannot vouch for itself.
    {"Checksum3", kChecksum3},
    {"Checksum4", kChecksum4},
};

class ChecksumEcuSubaruHitachiM32rCanStaleSlot : public ::testing::TestWithParam<StaleSlotCase>
{
};

TEST_P(ChecksumEcuSubaruHitachiM32rCanStaleSlot, RewritesTheSlotToTheZeroRomValue)
{
    bytes::Bytes rom(Rom::kRomSize, 0x00);
    bytes::WriteU32Be(rom, GetParam().address, 1);

    const ChecksumResult result = Rom::CalculateChecksumResult(Rom::Rom(rom));

    EXPECT_EQ(ReadSlots(result.rom_data), (Slots{0, 0, 0, 0, BalanceFor(0)}));
}

INSTANTIATE_TEST_SUITE_P(ChecksumPortable, ChecksumEcuSubaruHitachiM32rCanStaleSlot,
                         ::testing::ValuesIn(kStaleSlotCases), StaleSlotCaseName);

// Checksum 6 is compared but never written (#657), so a ROM only reaches
// Unchanged when its slot already holds the right value.
// Worked out here from the layout rather than taken from the algorithm.
std::uint16_t Checksum6(bytes::ByteView rom)
{
    std::uint8_t sum = 0;
    std::uint8_t xor_all = 0;
    for (std::size_t i = 0; i < rom.size(); ++i)
    {
        if (i < kChecksum6 || i >= kChecksum6 + 4)
        {
            sum = static_cast<std::uint8_t>(sum + rom[i]);
            xor_all ^= rom[i];
        }
    }
    return static_cast<std::uint16_t>((sum << 8U) | xor_all);
}

TEST(ChecksumPortable, ChecksumEcuSubaruHitachiM32rCanLeavesARomWithEveryChecksumHeldUnchanged)
{
    bytes::Bytes rom(Rom::kRomSize, 0x00);
    bytes::WriteU32Be(rom, 0x6000, 0x12345678);
    rom[0x20001] = 0x9a;
    // The excluded word's last two bytes are in no checksum; nonzero, they
    // show a window that strays into them.
    rom[kChecksum6 + 2] = 0xc3;
    rom[kChecksum6 + 3] = 0x3c;
    bytes::Bytes balanced = Rom::CalculateChecksumResult(Rom::Rom(rom)).rom_data;
    const std::uint16_t checksum_6 = Checksum6(balanced);
    // Both halves nonzero, so a slot read as data or a combination that
    // subtracts them shows.
    ASSERT_NE(checksum_6 >> 8U, 0);
    ASSERT_NE(checksum_6 & 0xffU, 0);
    bytes::WriteU16Be(balanced, kChecksum6, checksum_6);

    const ChecksumResult result = Rom::CalculateChecksumResult(Rom::Rom(balanced));

    EXPECT_EQ(result.status, ChecksumResult::Status::kUnchanged);
    EXPECT_EQ(result.rom_data, balanced);
}

} // namespace
