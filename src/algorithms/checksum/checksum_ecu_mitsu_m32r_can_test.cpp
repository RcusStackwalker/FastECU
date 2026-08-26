#include "src/algorithms/checksum/checksum_ecu_mitsu_m32r_can.h"

#include <gtest/gtest.h>

#include <cstdint>

#include "src/algorithms/protocol/bytes.h"

// The oracle below is a transcription of the ECU's own ROM CRC checker as it
// runs on a Colt CZT Z37A (47110032), read out of the annotated disassembly
// in the parent research repo. It is deliberately written from the M32R side
// rather than from ChecksumEcuMitsuM32rCan, so that "the corrected image
// satisfies the ECU" is a claim about the ECU and not a restatement of the
// module's own arithmetic.
//
//   sumx8()                    @ 0x0BA64  512-byte block sum, 32-bit BE words
//   rom_crc_finalize()         @ 0x4B0AC  subtracts the six correction words
//   rom_crc_finalized_check()  @ 0x4B0FC  faults unless the result is 0x5AA55AA5
//   flash_crc_check_block()    @ 0x4B1BC  pass 1: page walk over the whole area
//   flash_crc_check_block_2()  @ 0x4B310  pass 2: re-walks the special pages
//   in_range_50000_5a000()     @ 0x4B2B4
//   in_range_56000_5f0d0()     @ 0x4B2D8
//
// A CRC fault sets rom_crc_checker_flags bit 1, increments the guarded error
// counter and raises the ROM-checksum DTC, which is what keeps a mis-summed
// image out of userspace.

namespace
{

constexpr std::uint32_t kEcuTargetCrc = 0x5AA55AA5;
constexpr std::size_t kPageSize = 512;
constexpr std::size_t kBalanceSlot = 0x3FFC0;
constexpr std::size_t kAreaCodeOffset = 0x3FFCB;
constexpr std::size_t kCorrectionWords = 0x3FFCE;
constexpr std::size_t kExclusionFlag = 0x5013E;
constexpr std::size_t kFaultReportingFlag = 0x5013F;

// flash_crc_check_block's per-block call, sumx8(ptr, 16): sixteen unrolled
// passes over eight big-endian words each.
std::uint32_t ecuBlockSum(bytes::ByteView rom, std::size_t page)
{
    std::uint32_t sum = 0;
    for (std::size_t offset = page; offset < page + kPageSize; offset += 4)
    {
        sum += bytes::readU32Be(rom, offset);
    }
    return sum;
}

struct EcuCheckerPasses
{
    std::uint32_t finalized = 0;       // rom_crc_finalized after pass 1 wraps
    std::uint32_t specialPagesCrc = 0; // guarded_rom_special_pages_crc.d
    std::uint32_t secondPassCrc = 0;   // rom_check_crc2 after pass 2 wraps
};

EcuCheckerPasses runEcuRomCrcChecker(bytes::ByteView rom, std::size_t areaEnd)
{
    // flash5013e_u8: when clear, pass 1 skips 0x50000-0x5A000 entirely.
    // Transcribed for fidelity, but inert on this family -- the byte lives in
    // flash and 47110032 ships it as 0x01, so the branch is dead code in the
    // ROMs these protocols reach and the module does not model it.
    const bool sumEveryPage = rom[kExclusionFlag] != 0;

    EcuCheckerPasses passes;
    std::uint32_t romCheckCrc = 0;

    for (std::size_t page = 0; page < areaEnd; page += kPageSize)
    {
        const bool excluded = !sumEveryPage && page >= 0x50000 && page < 0x5A000;
        const std::uint32_t blockSum = excluded ? 0 : ecuBlockSum(rom, page);
        if (page >= 0x56000 && page < 0x5F0D0)
        {
            passes.specialPagesCrc += blockSum;
        }
        romCheckCrc += blockSum;
    }

    // rom_crc_finalize(): -u16[0x3FFCE], -u32[0x3FFD0 + 4i] for i < 5, +0xFFFF, -5.
    std::uint32_t finalized = romCheckCrc;
    finalized -= bytes::readU16Be(rom, kCorrectionWords);
    for (std::size_t index = 0; index < 5; ++index)
    {
        finalized -= bytes::readU32Be(rom, kCorrectionWords + 2 + (index * 4));
    }
    finalized += 0xFFFF;
    finalized -= 5;
    passes.finalized = finalized;

    // flash_crc_check_block_2() re-walks 0x56000 up to the first page start at
    // or past 0x5F0D0, which range_bounded_56000_5f0d0_default() wraps back to
    // 0x56000. rom_crc_finalized_check2() then requires the two agree.
    for (std::size_t page = 0x56000; page < 0x5F0D0; page += kPageSize)
    {
        passes.secondPassCrc += ecuBlockSum(rom, page);
    }

    return passes;
}

// True when the ECU would boot to userspace without raising the ROM-checksum
// DTC: pass 1 must finalize to 0x5AA55AA5 and pass 2 must agree with what
// pass 1 latched for the special pages.
bool ecuAcceptsRom(bytes::ByteView rom, std::size_t areaEnd)
{
    const EcuCheckerPasses passes = runEcuRomCrcChecker(rom, areaEnd);
    return passes.finalized == kEcuTargetCrc && passes.secondPassCrc == passes.specialPagesCrc;
}

// A 384 KiB image shaped like a Colt CZT ROM: 0xC2 area code, the six
// correction words erased, both checker flags set the way 47110032 ships
// them, and enough varied payload that the block sums are not degenerate.
bytes::Bytes syntheticColtRom(std::size_t size = 0x60000, std::uint8_t areaCode = 0xC2)
{
    bytes::Bytes rom(size, 0x00);
    for (std::size_t offset = 0; offset < rom.size(); ++offset)
    {
        rom[offset] = static_cast<bytes::Byte>((offset * 31) & 0xFF);
    }
    bytes::writeU32Be(rom, kBalanceSlot, 0xFFFFFFFF);
    rom[kAreaCodeOffset] = areaCode;
    bytes::writeU16Be(rom, kCorrectionWords, 0xFFFF);
    for (std::size_t index = 0; index < 5; ++index)
    {
        bytes::writeU32Be(rom, kCorrectionWords + 2 + (index * 4), 0xFFFFFFFF);
    }
    rom[kExclusionFlag] = 0x01;
    rom[kFaultReportingFlag] = 0x01;
    return rom;
}

// Balances a fixture through the oracle, so a test that needs an
// already-correct ROM does not obtain one from the code under test.
void balanceWithEcuModel(bytes::Bytes& rom, std::size_t areaEnd)
{
    bytes::writeU32Be(rom, kBalanceSlot, 0);
    const std::uint32_t finalized = runEcuRomCrcChecker(rom, areaEnd).finalized;
    bytes::writeU32Be(rom, kBalanceSlot, kEcuTargetCrc - finalized);
}

} // namespace

TEST(ChecksumEcuMitsuM32rCanTest, LeavesAnImageTheEcuAlreadyAcceptsUnchanged)
{
    bytes::Bytes rom = syntheticColtRom();
    balanceWithEcuModel(rom, 0x60000);
    ASSERT_TRUE(ecuAcceptsRom(rom, 0x60000));

    const ChecksumResult result = ChecksumEcuMitsuM32rCan::calculate_checksum_result(rom);

    EXPECT_EQ(result.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(result.romData, rom);
}

TEST(ChecksumEcuMitsuM32rCanTest, CorrectsAnImageTheEcuWouldRejectUntilItAccepts)
{
    const bytes::Bytes rom = syntheticColtRom();
    ASSERT_FALSE(ecuAcceptsRom(rom, 0x60000));

    const ChecksumResult result = ChecksumEcuMitsuM32rCan::calculate_checksum_result(rom);

    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_TRUE(ecuAcceptsRom(result.romData, 0x60000));
}

TEST(ChecksumEcuMitsuM32rCanTest, AreaCode0x50SumsTheFull512KiBTheEcuWalks)
{
    const bytes::Bytes rom = syntheticColtRom(0x80000, 0x50);
    ASSERT_FALSE(ecuAcceptsRom(rom, 0x80000));

    const ChecksumResult result = ChecksumEcuMitsuM32rCan::calculate_checksum_result(rom);

    EXPECT_EQ(result.status, ChecksumResult::Status::Corrected);
    EXPECT_TRUE(ecuAcceptsRom(result.romData, 0x80000));
}

TEST(ChecksumEcuMitsuM32rCanTest, UnrecognisedAreaCodeDisablesChecksumsAndTouchesNothing)
{
    const bytes::Bytes rom = syntheticColtRom(0x60000, 0x11);

    const ChecksumResult result = ChecksumEcuMitsuM32rCan::calculate_checksum_result(rom);

    EXPECT_EQ(result.status, ChecksumResult::Status::Disabled);
    EXPECT_EQ(result.romData, rom);
}

TEST(ChecksumEcuMitsuM32rCanTest, AreaCodeLargerThanTheFileDisablesChecksumsAndTouchesNothing)
{
    // 0x60 asks for a 1 MiB sweep, which a 384 KiB image cannot satisfy.
    const bytes::Bytes rom = syntheticColtRom(0x60000, 0x60);

    const ChecksumResult result = ChecksumEcuMitsuM32rCan::calculate_checksum_result(rom);

    EXPECT_EQ(result.status, ChecksumResult::Status::Disabled);
    EXPECT_EQ(result.romData, rom);
}

TEST(ChecksumEcuMitsuM32rCanTest, ImageTooSmallToCarryTheLayoutIsRejectedAsInvalidSize)
{
    const bytes::Bytes rom(0x1000, 0x00);

    const ChecksumResult result = ChecksumEcuMitsuM32rCan::calculate_checksum_result(rom);

    EXPECT_EQ(result.status, ChecksumResult::Status::InvalidSize);
    EXPECT_EQ(result.romData, rom);
}

TEST(ChecksumEcuMitsuM32rCanTest, CorrectionRewritesOnlyTheFourBytesOfTheBalanceSlot)
{
    const bytes::Bytes rom = syntheticColtRom();

    const ChecksumResult result = ChecksumEcuMitsuM32rCan::calculate_checksum_result(rom);

    ASSERT_EQ(result.status, ChecksumResult::Status::Corrected);
    ASSERT_EQ(result.romData.size(), rom.size());
    for (std::size_t offset = 0; offset < rom.size(); ++offset)
    {
        const bool inSlot = offset >= kBalanceSlot && offset < kBalanceSlot + 4;
        if (!inSlot)
        {
            ASSERT_EQ(result.romData[offset], rom[offset]) << "byte 0x" << std::hex << offset << " moved";
        }
    }
    // The slot sits inside the 0x08000-0x60000 window the Colt CAN protocols
    // write, and outside the 0x56000-0x5F0D0 pages the ECU's second pass
    // re-sums, so correcting it cannot disturb that check.
    EXPECT_GE(kBalanceSlot, 0x08000U);
    EXPECT_LT(kBalanceSlot + 4, 0x56000U);
}

TEST(ChecksumEcuMitsuM32rCanTest, CorrectingAnAlreadyCorrectedImageChangesNothingFurther)
{
    const ChecksumResult first = ChecksumEcuMitsuM32rCan::calculate_checksum_result(syntheticColtRom());
    ASSERT_EQ(first.status, ChecksumResult::Status::Corrected);

    const ChecksumResult second = ChecksumEcuMitsuM32rCan::calculate_checksum_result(first.romData);

    EXPECT_EQ(second.status, ChecksumResult::Status::Unchanged);
    EXPECT_EQ(second.romData, first.romData);
}
