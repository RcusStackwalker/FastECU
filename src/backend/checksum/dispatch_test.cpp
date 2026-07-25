#include "src/backend/checksum/dispatch.h"
#include <gtest/gtest.h>

using fastecu::checksum::apply_checksum_correction;
using fastecu::checksum::ChecksumSelection;
using Status = fastecu::checksum::ChecksumCorrectionOutcome::Status;

namespace
{
ChecksumSelection subaruSelection(std::string flash_method, std::string mcu_type,
                                  std::string rom_id = "39670016")
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
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_ecu_denso_sh7055", "SH7055"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(outcome.family_result->message, "Subaru Denso SH705x Checksum");
}

TEST(ApplyChecksumCorrection, Sh705xDieselTakesPriorityOverPlainSh7058Prefix)
{
    const bytes::Bytes rom(1024 * 1024, 0); // SH7058 romsize, 0x0FFB80 + 204 in bounds
    const auto outcome = apply_checksum_correction(
        rom, subaruSelection("sub_ecu_denso_sh7058_can_diesel", "SH7058"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    // Distinctive message proves this routed to SH705xDiesel, not the plain
    // SH7xxx branch both prefixes would otherwise match.
    EXPECT_EQ(outcome.family_result->message, "Subaru Denso SH705x Checksum");
}

TEST(ApplyChecksumCorrection, HitachiM32rKline_RomIdStartingWith3RoutesToKlineFamily)
{
    const bytes::Bytes rom(524288, 0); // M32R_512KB romsize
    const auto outcome = apply_checksum_correction(
        rom, subaruSelection("sub_ecu_hitachi_m32r_kline", "M32R_512KB", "39670016"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R K-Line ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiM32rKline_RomIdStartingWith4RoutesToCanFamily)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = apply_checksum_correction(
        rom, subaruSelection("sub_ecu_hitachi_m32r_kline", "M32R_512KB", "47110032"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiM32rKline_RomIdStartingWith6RoutesToCanFamily)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = apply_checksum_correction(
        rom, subaruSelection("sub_ecu_hitachi_m32r_kline", "M32R_512KB", "63520003"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiM32rKline_UnrecognizedRomIdIsANoOpWithModuleAvailable)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = apply_checksum_correction(
        rom, subaruSelection("sub_ecu_hitachi_m32r_kline", "M32R_512KB", "51234567"));

    // FamilyRan (module "available", no warning dialog), but no family
    // actually ran -- matches legacy checksum_correction exactly.
    EXPECT_EQ(outcome.status, Status::FamilyRan);
    EXPECT_FALSE(outcome.family_result.has_value());
}

TEST(ApplyChecksumCorrection, HitachiM32rCanRoutesForPlainFlashMethod)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiSh7058RoutesCorrectly)
{
    const bytes::Bytes rom(1024 * 1024, 0);
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_ecu_hitachi_sh7058_can", "SH7058"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi SH7058 CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, HitachiSh72543rRoutesCorrectly)
{
    const bytes::Bytes rom(2 * 1024 * 1024, 0);
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_ecu_hitachi_sh72543r", "SH72543R"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi SH72543r ECU Checksum");
}

TEST(ApplyChecksumCorrection, TcuDensoSh7055RoutesCorrectly)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_tcu_denso_sh7055_can", "SH7055"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Denso SH7055 TCU Checksum");
}

TEST(ApplyChecksumCorrection, TcuHitachiM32rCanRoutesForCanFlashMethod)
{
    const bytes::Bytes rom(65536, 0); // M3779x romsize
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_tcu_hitachi_m32r_can", "M3779x"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, TcuHitachiM32rCanRoutesForKlineFlashMethodToo)
{
    // Legacy checksum_correction routes both "sub_tcu_hitachi_m32r_can" and
    // "sub_tcu_hitachi_m32r_kline" to the same family class
    // (file_actions.cpp:2311-2321) -- not a typo, preserved verbatim.
    const bytes::Bytes rom(65536, 0);
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_tcu_hitachi_m32r_kline", "M3779x"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, TcuMitsuMh8104RoutesCorrectly)
{
    const bytes::Bytes rom(524288, 0); // MH8104 romsize
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_tcu_cvt_mitsu_mh8104_can", "MH8104"));

    ASSERT_EQ(outcome.status, Status::FamilyRan);
    ASSERT_TRUE(outcome.family_result.has_value());
    EXPECT_EQ(outcome.family_result->message, "Subaru Hitachi M32R K-Line/CAN ECU Checksum");
}

TEST(ApplyChecksumCorrection, UnknownMcuTypeReturnsUnknownMcuTypeStatus)
{
    const bytes::Bytes rom(100, 0);
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_ecu_hitachi_m32r_can", "M32170"));

    EXPECT_EQ(outcome.status, Status::UnknownMcuType);
    EXPECT_FALSE(outcome.family_result.has_value());
}

TEST(ApplyChecksumCorrection, BadRomSizeReturnsBadRomSizeStatus)
{
    const bytes::Bytes rom(100, 0); // wrong size for M32R_512KB
    const auto outcome = apply_checksum_correction(rom, subaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB"));

    EXPECT_EQ(outcome.status, Status::BadRomSize);
    EXPECT_FALSE(outcome.family_result.has_value());
}

TEST(ApplyChecksumCorrection, NonSubaruMakeReturnsNoModuleForProtocol)
{
    const bytes::Bytes rom(524288, 0);
    ChecksumSelection selection = subaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB");
    selection.make = "Mitsubishi";
    const auto outcome = apply_checksum_correction(rom, selection);

    EXPECT_EQ(outcome.status, Status::NoModuleForProtocol);
}

TEST(ApplyChecksumCorrection, ChecksumFlagNoReturnsNoModuleForProtocol)
{
    const bytes::Bytes rom(524288, 0);
    ChecksumSelection selection = subaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB");
    selection.checksum_flag = "no";
    const auto outcome = apply_checksum_correction(rom, selection);

    EXPECT_EQ(outcome.status, Status::NoModuleForProtocol);
}

TEST(ApplyChecksumCorrection, ChecksumFlagNaReturnsNoModuleForProtocol)
{
    // Dispatch treats "n/a" the same as "no" (neither is "yes"); the
    // no-vs-n/a distinction that changes whether the warning dialog fires is
    // an adapter-level decision (Task 3), not this function's concern.
    const bytes::Bytes rom(524288, 0);
    ChecksumSelection selection = subaruSelection("sub_ecu_hitachi_m32r_can", "M32R_512KB");
    selection.checksum_flag = "n/a";
    const auto outcome = apply_checksum_correction(rom, selection);

    EXPECT_EQ(outcome.status, Status::NoModuleForProtocol);
}

TEST(ApplyChecksumCorrection, UnmatchedFlashMethodReturnsNoModuleForProtocol)
{
    const bytes::Bytes rom(524288, 0);
    const auto outcome = apply_checksum_correction(rom, subaruSelection("does_not_exist", "M32R_512KB"));

    EXPECT_EQ(outcome.status, Status::NoModuleForProtocol);
}
