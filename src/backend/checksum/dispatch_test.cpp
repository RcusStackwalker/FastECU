#include "src/backend/checksum/dispatch.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>

#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/memory/testing/memory_views.h"
#include "src/algorithms/protocol/testing/byte_matchers.h"

using fastecu::checksum::ApplyChecksumCorrection;
using fastecu::checksum::ChecksumSelection;
using Status = fastecu::checksum::ChecksumCorrectionOutcome::Status;
using ::testing::HasSubstr;
namespace memory = fastecu::memory;

namespace
{
memory::MemoryImage IdentityOf(const bytes::Bytes& rom)
{
    return memory::testing::ImageAt(memory::FlashAddress{0}, rom);
}

memory::AddressRange<memory::FlashSpace> Range(std::uint32_t start, std::uint32_t size)
{
    return memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size}).value();
}

ChecksumSelection SubaruSelection(std::string flash_method, std::string mcu_type, std::string rom_id = "39670016")
{
    ChecksumSelection s;
    s.make = "Subaru";
    s.checksum_flag = "yes";
    s.flash_method = std::move(flash_method);
    s.mcu_type = std::move(mcu_type);
    s.rom_id = std::move(rom_id);
    return s;
}
} // namespace

TEST(ApplyChecksumCorrection, DensoSh7xxxRoutesForPlainSh7055)
{
    const bytes::Bytes rom(524288, 0); // SH7055 romsize, 0x07FB80 + 204 in bounds
    const auto outcome = ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_sh7055", "SH7055"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(outcome.family_result->message, "Subaru Denso SH705x Checksum");
}

TEST(ApplyChecksumCorrection, Sh705xDieselTakesPriorityOverPlainSh7058Prefix)
{
    const bytes::Bytes rom(1024UZ * 1024, 0); // SH7058 romsize, 0x0FFB80 + 204 in bounds
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_sh7058_can_diesel", "SH7058"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    // Distinctive message proves this routed to SH705xDiesel, not the plain
    // SH7xxx branch both prefixes would otherwise match.
    EXPECT_EQ(outcome.family_result->message, "Subaru Denso SH705x Checksum");
}

TEST(ApplyChecksumCorrection, PrefixRoutesAlsoAcceptFlashMethodSuffixes)
{
    const bytes::Bytes rom(1024UZ * 1024, 0);
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_sh7058_can_diesel_variant", "SH7058"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Denso SH705x Checksum");
}

TEST(ApplyChecksumCorrection, HitachiM32rKline_RomIdStartingWith3RoutesToKlineFamily)
{
    const bytes::Bytes rom(524288, 0); // M32R_512KB romsize
    const auto outcome = ApplyChecksumCorrection(
        IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_m32r_kline", "M32R_512KB", "39670016"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R K-Line ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiM32rKline_RomIdStartingWith4RoutesToCanFamily)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = ApplyChecksumCorrection(
        IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_m32r_kline", "M32R_512KB", "47110032"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiM32rKline_RomIdStartingWith6RoutesToCanFamily)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = ApplyChecksumCorrection(
        IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_m32r_kline", "M32R_512KB", "63520003"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiM32rKline_UnrecognizedRomIdIsANoOpWithModuleAvailable)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = ApplyChecksumCorrection(
        IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_m32r_kline", "M32R_512KB", "51234567"));

    // FamilyRan (module "available", no warning dialog), but no family
    // actually ran -- matches legacy checksum_correction exactly.
    EXPECT_EQ(outcome.status, Status::kFamilyRan);
    EXPECT_FALSE(outcome.family_result.has_value());
}

TEST(ApplyChecksumCorrection, HitachiM32rCanRoutesForPlainFlashMethod)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiSh7058RoutesCorrectly)
{
    const bytes::Bytes rom(1024UZ * 1024, 0);
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_sh7058_can", "SH7058"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi SH7058 CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiSh72543rRoutesCorrectly)
{
    const bytes::Bytes rom(2UZ * 1024 * 1024, 0);
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_sh72543r", "SH72543R"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi SH72543r ECU Checksum");
}

TEST(ApplyChecksumCorrection, TcuDensoSh7055RoutesCorrectly)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_tcu_denso_sh7055_can", "SH7055"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Denso SH7055 TCU Checksum");
}

TEST(ApplyChecksumCorrection, TcuHitachiM32rCanRoutesForCanFlashMethod)
{
    const bytes::Bytes rom(65536, 0); // M3779x romsize
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_tcu_hitachi_m32r_can", "M3779x"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, TcuHitachiM32rCanRoutesForKlineFlashMethodToo)
{
    // Legacy checksum_correction routes both "sub_tcu_hitachi_m32r_can" and
    // "sub_tcu_hitachi_m32r_kline" to the same family class
    // (file_actions.cpp:2311-2321) -- not a typo, preserved verbatim.
    const bytes::Bytes rom(65536, 0);
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_tcu_hitachi_m32r_kline", "M3779x"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, TcuMitsuMh8104RoutesCorrectly)
{
    const bytes::Bytes rom(524288, 0); // MH8104 romsize
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_tcu_cvt_mitsu_mh8104_can", "MH8104"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, UnknownMcuTypeReturnsUnknownMcuTypeStatus)
{
    const bytes::Bytes rom(100, 0);
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_m32r_can", "M32170"));

    EXPECT_EQ(outcome.status, Status::kUnknownMcuType);
    EXPECT_FALSE(outcome.family_result.has_value());
}

TEST(ApplyChecksumCorrection, BadRomSizeReturnsBadRomSizeStatus)
{
    const bytes::Bytes rom(100, 0); // wrong size for M32R_512KB
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB"));

    EXPECT_EQ(outcome.status, Status::kBadRomSize);
    EXPECT_FALSE(outcome.family_result.has_value());
}

TEST(ApplyChecksumCorrection, NonSubaruMakeReturnsNoModuleForProtocol)
{
    const bytes::Bytes rom(524288, 0);
    ChecksumSelection selection = SubaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB");
    selection.make = "Mitsubishi";
    const auto outcome = ApplyChecksumCorrection(IdentityOf(rom), selection);

    EXPECT_EQ(outcome.status, Status::kNoModuleForProtocol);
}

TEST(ApplyChecksumCorrection, ChecksumFlagNoReturnsNoModuleForProtocol)
{
    const bytes::Bytes rom(524288, 0);
    ChecksumSelection selection = SubaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB");
    selection.checksum_flag = "no";
    const auto outcome = ApplyChecksumCorrection(IdentityOf(rom), selection);

    EXPECT_EQ(outcome.status, Status::kNoModuleForProtocol);
}

TEST(ApplyChecksumCorrection, ChecksumFlagNaReturnsNoModuleForProtocol)
{
    // Dispatch treats "n/a" the same as "no" (neither is "yes"); the
    // no-vs-n/a distinction that changes whether the warning dialog fires is
    // an adapter-level decision (Task 3), not this function's concern.
    const bytes::Bytes rom(524288, 0);
    ChecksumSelection selection = SubaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB");
    selection.checksum_flag = "n/a";
    const auto outcome = ApplyChecksumCorrection(IdentityOf(rom), selection);

    EXPECT_EQ(outcome.status, Status::kNoModuleForProtocol);
}

TEST(ApplyChecksumCorrection, UnmatchedFlashMethodReturnsNoModuleForProtocol)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("does_not_exist", "M32R_512KB"));

    EXPECT_EQ(outcome.status, Status::kNoModuleForProtocol);
}

// The remaining Denso branches below all route through one of the two
// already-exercised family classes (SH7xxx or SH705xDiesel); each test here
// exists to prove ITS specific flash_method prefix reaches dispatch_family's
// chain at all (routing), not to re-verify algorithm math already covered by
// DensoSh7xxxRoutesForPlainSh7055/Sh705xDieselTakesPriorityOverPlainSh7058Prefix
// and by src/algorithms/checksum:checksum_test.

TEST(ApplyChecksumCorrection, Sh7058sDieselDensocanRoutesToSh705xDiesel)
{
    const bytes::Bytes rom(1024UZ * 1024, 0); // SH7058 romsize, 0x0FFB80 + 204 in bounds
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_sh7058s_diesel_densocan", "SH7058"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
}

TEST(ApplyChecksumCorrection, PlainSh7058RoutesToSh7xxx)
{
    const bytes::Bytes rom(1024UZ * 1024, 0); // SH7058 romsize, 0x0FFB80 + 204 in bounds
    const auto outcome = ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_sh7058", "SH7058"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
}

TEST(ApplyChecksumCorrection, Sh72531CanRoutesToSh7xxx)
{
    const bytes::Bytes rom(1280UZ * 1024, 0); // SH72531 romsize, 0x13F500 + 204 in bounds
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_sh72531_can", "SH72531"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
}

TEST(ApplyChecksumCorrection, N83m4mCanRoutesToSh7xxxWithNegativeOffset)
{
    const bytes::Bytes rom(3984UZ * 1024, 0); // N83M_4MB romsize, 0x3E3E00 + 204 in bounds
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_1n83m_4m_can", "N83M_4MB"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
}

TEST(ApplyChecksumCorrection, N83m1_5mCanRoutesToSh7xxxWithNegativeOffset)
{
    // Uses N83M_4MB, not the "naturally" paired N83M_1_5MB: this
    // flash_method's hardcoded area_start (0x183E00) is 1,588,736 bytes in,
    // which exceeds N83M_1_5MB's own romsize (1,523,712) -- a pre-existing
    // legacy quirk this port preserves verbatim, not something to fix here.
    // N83M_4MB is large enough to prove routing without hitting that quirk.
    const bytes::Bytes rom(3984UZ * 1024, 0);
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_1n83m_1_5m_can", "N83M_4MB"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
}

TEST(ApplyChecksumCorrection, Sh7059CanDieselRoutesToSh705xDiesel)
{
    const bytes::Bytes rom(1536UZ * 1024, 0); // SH7059d romsize, 0x17FB80 + 204 in bounds
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_sh7059_can_diesel", "SH7059d"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
}

TEST(ApplyChecksumCorrection, Sh7059DieselDensocanRoutesToSh705xDiesel)
{
    const bytes::Bytes rom(1536UZ * 1024, 0); // SH7059d romsize, 0x17FB80 + 204 in bounds
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_sh7059_diesel_densocan", "SH7059d"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
}

TEST(ApplyChecksumCorrection, Sh72543CanDieselRoutesToSh705xDiesel)
{
    const bytes::Bytes rom(2UZ * 1024 * 1024, 0); // SH72543d romsize, 0x1FF800 + 204 in bounds
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_denso_sh72543_can_diesel", "SH72543d"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
}

TEST(ApplyChecksumCorrection, TcuDensoSh7058CanRoutesToSh7xxx)
{
    const bytes::Bytes rom(1024UZ * 1024, 0); // SH7058 romsize, 0x0FFB80 + 204 in bounds
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_tcu_denso_sh7058_can", "SH7058"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kCorrected);
}

namespace
{
// M32R_384KB_1block romsize, shaped enough that the Colt module recognises
// the layout: 0xC2 selects the 384 KiB sweep and flash5013e_u8 is set the way
// stock 47110032 ships it.
bytes::Bytes ColtRom(std::size_t size = 0x60000)
{
    bytes::Bytes rom(size, 0x00);
    rom[0x3FFCB] = 0xC2;
    rom[0x5013E] = 0x01;
    return rom;
}

ChecksumSelection ColtSelection(std::string flash_method, std::string mcu_type = "M32R_384KB_1block")
{
    ChecksumSelection s;
    s.make = "Mitsubishi";
    s.checksum_flag = "yes";
    s.flash_method = std::move(flash_method);
    s.mcu_type = std::move(mcu_type);
    s.rom_id = "47110032";
    return s;
}
} // namespace

TEST(ApplyChecksumCorrection, AllFourColtCanProtocolsRouteToTheMitsuM32rCanFamily)
{
    for (const auto& [flash_method, mcu_type, size] :
         std::to_array<std::tuple<std::string_view, std::string_view, std::size_t>>({
             {"mitsu_ecu_m32r_can", "M32R_384KB_1block", 0x60000},
             {"mitsu_ecu_m32r_can_vendor_ext", "M32R_384KB_1block", 0x60000},
             {"mitsu_ecu_m32r_can_512kb", "M32R_512KB_1block", 0x80000},
             {"mitsu_ecu_m32r_can_vendor_ext_512kb", "M32R_512KB_1block", 0x80000},
         }))
    {
        const auto outcome = ApplyChecksumCorrection(IdentityOf(ColtRom(size)),
                                                     ColtSelection(std::string(flash_method), std::string(mcu_type)));

        ASSERT_EQ(outcome.status, Status::kFamilyRan) << flash_method;
        ASSERT_TRUE(outcome.family_result.has_value()) << flash_method;
        EXPECT_EQ(outcome.family_result->message, "Mitsubishi M32R CAN ECU Checksum") << flash_method;
    }
}

TEST(ApplyChecksumCorrection, ColtFlashMethodUnderSubaruMakeFindsNoModule)
{
    ChecksumSelection selection = ColtSelection("mitsu_ecu_m32r_can");
    selection.make = "Subaru";

    const auto outcome = ApplyChecksumCorrection(IdentityOf(ColtRom()), selection);

    EXPECT_EQ(outcome.status, Status::kNoModuleForProtocol);
}

TEST(ApplyChecksumCorrection, MitsubishiMakeDoesNotOpenTheKlineMutDmaProtocol)
{
    // Adding a Mitsubishi route must not make every mitsu_* protocol eligible.
    const auto outcome = ApplyChecksumCorrection(IdentityOf(ColtRom()), ColtSelection("mitsu_ecu_m32r_kline_mut_dma"));

    EXPECT_EQ(outcome.status, Status::kNoModuleForProtocol);
}

TEST(HasRoute, MatchesExactlyTheRoutesCorrectionDispatches)
{
    EXPECT_TRUE(fastecu::checksum::HasRoute("Subaru", "sub_ecu_denso_sh7058_can"));
    EXPECT_TRUE(fastecu::checksum::HasRoute("Subaru", "sub_ecu_denso_sh7058_can_cobb")); // prefix routes take suffixes
    EXPECT_TRUE(fastecu::checksum::HasRoute("Mitsubishi", "mitsu_ecu_m32r_can_512kb"));
    EXPECT_FALSE(fastecu::checksum::HasRoute("Mitsubishi", "sub_ecu_denso_sh7058_can")); // a route belongs to one make
    EXPECT_FALSE(fastecu::checksum::HasRoute("Subaru", "sub_ecu_mitsu_m32r_kline"));
    EXPECT_FALSE(fastecu::checksum::HasRoute("Mitsubishi", "mitsu_ecu_m32r_kline_mut_dma"));
}

// SH72543R rebalances one word at ECU 0x1FFFFE. Two 1 MiB blocks in swapped
// file order put that word at file offset 0x0FFFFE.
TEST(ApplyChecksumCorrection, WritesCorrectedBytesBackAtTheirFileOffsets)
{
    const std::array blocks{
        memory::MemoryBlock{.range = Range(0, 0x100000),
                            .backing = memory::FileBacking{.offset = memory::FileOffset{0x100000}},
                            .writability = memory::Writability::kWritable},
        memory::MemoryBlock{.range = Range(0x100000, 0x100000),
                            .backing = memory::FileBacking{.offset = memory::FileOffset{0}},
                            .writability = memory::Writability::kWritable},
    };
    const auto map = memory::MemoryMap::Create(blocks, memory::ByteCount{0x200000});
    ASSERT_TRUE(map.has_value());
    const auto image = memory::MemoryImage::Create(*map, bytes::Bytes(0x200000, 0));
    ASSERT_TRUE(image.has_value());

    const auto outcome = ApplyChecksumCorrection(*image, SubaruSelection("sub_ecu_hitachi_sh72543r", "SH72543R"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.corrected_file.has_value());
    EXPECT_EQ(bytes::ReadU16Be(*outcome.corrected_file, 0x0FFFFE), 0x5AA5U);
    EXPECT_EQ(bytes::ReadU16Be(*outcome.corrected_file, 0x1FFFFE), 0U);
}

TEST(ApplyChecksumCorrection, RefusesACorrectionThatWritesReadOnlyMemory)
{
    const memory::MemoryImage image =
        memory::testing::ImageAt(memory::FlashAddress{0}, bytes::Bytes(0x200000, 0), memory::Writability::kReadOnly);

    const auto outcome = ApplyChecksumCorrection(image, SubaruSelection("sub_ecu_hitachi_sh72543r", "SH72543R"));

    ASSERT_EQ(outcome.status, Status::kFamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kUnsupportedRom);
    EXPECT_THAT(outcome.family_result->message,
                HasSubstr("Checksum correction would change ROM bytes the protocol cannot write"));
    EXPECT_FALSE(outcome.corrected_file.has_value());
}

TEST(ApplyChecksumCorrection, AnUnchangedResultHandsBackTheFileAsItWas)
{
    bytes::Bytes rom(0x200000, 0);
    bytes::WriteU16Be(rom, 0x1FFFFE, 0x5AA5); // a zero ROM balanced by hand
    const auto outcome =
        ApplyChecksumCorrection(IdentityOf(rom), SubaruSelection("sub_ecu_hitachi_sh72543r", "SH72543R"));

    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::kUnchanged);
    ASSERT_TRUE(outcome.corrected_file.has_value());
    EXPECT_THAT(*outcome.corrected_file, test_bytes::BytesEq(rom));
}

TEST(ApplyChecksumCorrection, AMapWithUnmappedAddressesIsABadRomSize)
{
    const std::array blocks{
        memory::MemoryBlock{.range = Range(0, 0x100000),
                            .backing = memory::FileBacking{},
                            .writability = memory::Writability::kWritable},
        memory::MemoryBlock{.range = Range(0x180000, 0x100000),
                            .backing = memory::FileBacking{.offset = memory::FileOffset{0x100000}},
                            .writability = memory::Writability::kWritable},
    };
    const auto map = memory::MemoryMap::Create(blocks, memory::ByteCount{0x200000});
    ASSERT_TRUE(map.has_value());
    const auto image = memory::MemoryImage::Create(*map, bytes::Bytes(0x200000, 0));
    ASSERT_TRUE(image.has_value());

    EXPECT_EQ(ApplyChecksumCorrection(*image, SubaruSelection("sub_ecu_hitachi_sh72543r", "SH72543R")).status,
              Status::kBadRomSize);
}
