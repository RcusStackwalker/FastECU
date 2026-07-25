#include "src/backend/checksum/legacy_checksum_adapter.h"

#include <gtest/gtest.h>

using fastecu::checksum::ChecksumSelection;
using fastecu::checksum::LegacyChecksumAdapter;
using fastecu::checksum::LegacyChecksumAdapterResult;

namespace
{

class TestableChecksumAdapter : public LegacyChecksumAdapter
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

} // namespace

TEST(LegacyChecksumAdapterTest, DecliningGateReturnsUnchangedWithNoFamilyDialog)
{
    TestableChecksumAdapter adapter;
    adapter.proceedWithoutDefinitionAnswer = false;
    const bytes::Bytes rom(524288, 0);

    LegacyChecksumAdapterResult result =
        adapter.checksum_correction(rom, false, false, subaruM32rKlineSelection(), nullptr);

    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_FALSE(result.canceled_due_to_missing_module);
    EXPECT_EQ(adapter.familyResultDialogCount, 0);
}

TEST(LegacyChecksumAdapterTest, AcceptingGateWithoutLinkedDefinitionCorrectsRom)
{
    TestableChecksumAdapter adapter;
    const bytes::Bytes rom(524288, 0);

    LegacyChecksumAdapterResult result =
        adapter.checksum_correction(rom, false, false, subaruM32rKlineSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(adapter.familyResultDialogCount, 1);
    EXPECT_EQ(adapter.lastFamilyResult.status, ChecksumResult::Status::Corrected);
    EXPECT_EQ(adapter.lastFamilyResult.message, "Subaru Hitachi M32R K-Line ECU Checksum");
}

TEST(LegacyChecksumAdapterTest, GateNotConsultedWhenDefinitionAlreadyLinked)
{
    TestableChecksumAdapter adapter;
    adapter.proceedWithoutDefinitionAnswer = false; // would abort if the gate were (wrongly) shown
    const bytes::Bytes rom(524288, 0);

    LegacyChecksumAdapterResult result =
        adapter.checksum_correction(rom, true, false, subaruM32rKlineSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
}

TEST(LegacyChecksumAdapterTest, BadRomSizeShowsDialogAndMakesNoCorrection)
{
    TestableChecksumAdapter adapter;
    const bytes::Bytes rom(4096, 0); // wrong size for M32R_512KB

    LegacyChecksumAdapterResult result =
        adapter.checksum_correction(rom, true, false, subaruM32rKlineSelection(), nullptr);

    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_EQ(adapter.badRomSizeDialogCount, 1);
}

TEST(LegacyChecksumAdapterTest, NoModuleWithChecksumFlagNoAsksNothingAndDoesNotCancel)
{
    TestableChecksumAdapter adapter;
    ChecksumSelection selection = subaruM32rKlineSelection();
    selection.checksum_flag = "no";
    const bytes::Bytes rom(524288, 0);

    LegacyChecksumAdapterResult result = adapter.checksum_correction(rom, true, false, selection, nullptr);

    EXPECT_FALSE(result.canceled_due_to_missing_module);
}

TEST(LegacyChecksumAdapterTest, NoModuleWithChecksumFlagNaAsksAndRespectsCancel)
{
    TestableChecksumAdapter adapter;
    adapter.cancelWithoutModuleAnswer = true;
    ChecksumSelection selection = subaruM32rKlineSelection();
    selection.checksum_flag = "n/a";
    const bytes::Bytes rom(524288, 0);

    LegacyChecksumAdapterResult result = adapter.checksum_correction(rom, true, false, selection, nullptr);

    EXPECT_TRUE(result.canceled_due_to_missing_module);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
