#include "src/ui/desktop/checksum/checksum_correction_command.h"

#include <QWidget>
#include <gtest/gtest.h>

// ChecksumResult is in the global namespace -- see the namespace note above.
using fastecu::checksum::ChecksumSelection;
using fastecu::ui::ChecksumCorrectionCommand;
using fastecu::ui::ChecksumCorrectionResult;

namespace
{

class TestableChecksumCommand : public ChecksumCorrectionCommand
{
  public:
    bool proceedWithoutDefinitionAnswer = true; // "DO IT!" by default
    bool cancelWithoutModuleAnswer = false;     // "OK" (proceed) by default
    int badRomSizeDialogCount = 0;
    int familyResultDialogCount = 0;
    ChecksumResult lastFamilyResult;

  protected:
    bool confirmProceedWithoutDefinition(QWidget *) override
    {
        return proceedWithoutDefinitionAnswer;
    }
    void showBadRomSizeDialog(QWidget *) override
    {
        ++badRomSizeDialogCount;
    }
    bool confirmProceedWithoutChecksumModule() override
    {
        return cancelWithoutModuleAnswer;
    }
    void showFamilyResultDialog(const ChecksumResult& family_result) override
    {
        ++familyResultDialogCount;
        lastFamilyResult = family_result;
    }
};

ChecksumSelection subaruM32rKlineSelection()
{
    ChecksumSelection selection;
    selection.make = "Subaru";
    selection.checksum_flag = "yes";
    selection.flash_method = "sub_ecu_hitachi_m32r_kline";
    selection.mcu_type = "M32R_512KB";
    selection.rom_id = "39670016";
    return selection;
}

ChecksumSelection subaruDensoSh7058DieselSelection()
{
    ChecksumSelection selection;
    selection.make = "Subaru";
    selection.checksum_flag = "yes";
    selection.flash_method = "sub_ecu_denso_sh7058_can_diesel";
    selection.mcu_type = "SH7058";
    return selection;
}

} // namespace

TEST(ChecksumCorrectionCommand, DecliningGateReturnsUnchangedWithNoFamilyDialog)
{
    TestableChecksumCommand command;
    command.proceedWithoutDefinitionAnswer = false;
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(rom, false, false, subaruM32rKlineSelection(), nullptr);

    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_FALSE(result.canceled_due_to_missing_module);
    EXPECT_EQ(command.familyResultDialogCount, 0);
}

TEST(ChecksumCorrectionCommand, AcceptingGateWithoutLinkedDefinitionCorrectsRom)
{
    TestableChecksumCommand command;
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(rom, false, false, subaruM32rKlineSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(command.familyResultDialogCount, 1);
    EXPECT_EQ(command.lastFamilyResult.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(command.lastFamilyResult.message, "Subaru Hitachi M32R K-Line ECU Checksum");
}

TEST(ChecksumCorrectionCommand, GateNotConsultedWhenDefinitionAlreadyLinked)
{
    TestableChecksumCommand command;
    command.proceedWithoutDefinitionAnswer = false; // would abort if the gate were (wrongly) shown
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(rom, true, false, subaruM32rKlineSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
}

TEST(ChecksumCorrectionCommand, DisabledDieselChecksumPreservesRomData)
{
    TestableChecksumCommand command;
    bytes::Bytes rom(1024uz * 1024, 0);
    bytes::writeU32Be(rom, 0x0FFB88, 0x5AA5A55A);

    const ChecksumCorrectionResult result = command.run(rom, true, false, subaruDensoSh7058DieselSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(*result.corrected_rom_data, rom);
    EXPECT_EQ(command.lastFamilyResult.status, ChecksumResult::Status::Disabled);
}

TEST(ChecksumCorrectionCommand, BadRomSizeShowsDialogAndMakesNoCorrection)
{
    TestableChecksumCommand command;
    const bytes::Bytes rom(4096, 0); // wrong size for M32R_512KB

    ChecksumCorrectionResult result = command.run(rom, true, false, subaruM32rKlineSelection(), nullptr);

    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_EQ(command.badRomSizeDialogCount, 1);
}

TEST(ChecksumCorrectionCommand, NoModuleWithChecksumFlagNoAsksNothingAndDoesNotCancel)
{
    TestableChecksumCommand command;
    ChecksumSelection selection = subaruM32rKlineSelection();
    selection.checksum_flag = "no";
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(rom, true, false, selection, nullptr);

    EXPECT_FALSE(result.canceled_due_to_missing_module);
}

TEST(ChecksumCorrectionCommand, NoModuleWithChecksumFlagNaAsksAndRespectsCancel)
{
    TestableChecksumCommand command;
    command.cancelWithoutModuleAnswer = true;
    ChecksumSelection selection = subaruM32rKlineSelection();
    selection.checksum_flag = "n/a";
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(rom, true, false, selection, nullptr);

    EXPECT_TRUE(result.canceled_due_to_missing_module);
}

TEST(ChecksumCorrectionCommand, UnknownMcuTypeReturnsUnmodifiedRomAndRunsNoDialog)
{
    // "M32170" is sub_ecu_mitsu_m32r_can's real, currently shipped <mcu>
    // value in resources/shared/config/protocols.cfg; it has no
    // flashdevices[] entry. Formerly checksum_correction's early return.
    TestableChecksumCommand command;
    ChecksumSelection selection = subaruM32rKlineSelection();
    selection.mcu_type = "M32170";

    const bytes::Bytes rom(100, bytes::Byte{0});
    const ChecksumCorrectionResult result = command.run(bytes::ByteView(rom), true, false, selection, nullptr);

    EXPECT_TRUE(result.unknown_mcu_type);
    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_EQ(command.badRomSizeDialogCount, 0);
    EXPECT_EQ(command.familyResultDialogCount, 0);
}

TEST(ChecksumCorrectionCommand, ValidMcuCorrectsRomAndReturnsChangedBytes)
{
    TestableChecksumCommand command;
    ChecksumSelection selection;
    selection.make = "Subaru";
    selection.checksum_flag = "yes";
    selection.flash_method = "sub_ecu_denso_sh7055";
    selection.mcu_type = "SH7055";
    selection.rom_id = "39670016";

    const bytes::Bytes rom(524288, bytes::Byte{0}); // SH7055 romsize -> Corrected
    const ChecksumCorrectionResult result = command.run(bytes::ByteView(rom), true, false, selection, nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(result.corrected_rom_data->size(), 524288u);
    EXPECT_NE(*result.corrected_rom_data, rom);
    EXPECT_EQ(command.familyResultDialogCount, 1);
}
