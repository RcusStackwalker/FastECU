#include "src/backend/checksum/legacy_checksum_adapter.h"

#include <QApplication>
#include <QTimer>
#include <QWidget>
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

// TestableChecksumAdapter above overrides every dialog seam, so none of
// LegacyChecksumAdapter's own QMessageBox-showing bodies ever runs under
// test. This adapter instead calls through to the REAL base-class
// implementation for exactly one seam at a time (controlled by the
// passthrough* flags below) and keeps the other three canned/no-op, so a
// test exercises one real dialog in isolation rather than risking a second,
// unhandled modal cascading from the same call.
class PassthroughChecksumAdapter : public LegacyChecksumAdapter
{
  public:
    bool passthroughDefinitionGate = false;
    bool passthroughBadRomSize = false;
    bool passthroughNoModule = false;
    bool passthroughFamilyResult = false;

  protected:
    bool confirmProceedWithoutDefinition(QWidget *parent) override
    {
        if (passthroughDefinitionGate)
            return LegacyChecksumAdapter::confirmProceedWithoutDefinition(parent);
        return true;
    }
    void showBadRomSizeDialog(QWidget *parent) override
    {
        if (passthroughBadRomSize)
            LegacyChecksumAdapter::showBadRomSizeDialog(parent);
    }
    bool confirmProceedWithoutChecksumModule() override
    {
        if (passthroughNoModule)
            return LegacyChecksumAdapter::confirmProceedWithoutChecksumModule();
        return false;
    }
    void showFamilyResultDialog(const ChecksumResult& family_result) override
    {
        if (passthroughFamilyResult)
            LegacyChecksumAdapter::showFamilyResultDialog(family_result);
    }
};

// Arms a one-shot timer that closes whatever modal widget is active once the
// call under test starts its nested event loop. Used instead of
// tests/expected_message_box.h's ExpectedMessageBoxCloser: that helper calls
// QDialog::accept()/reject() directly, which does not set QMessageBox's
// clickedButton() for the custom-role buttons these dialogs use (only a real
// button click does) -- fine here, since these tests exist to exercise the
// real dialog-construction code paths for coverage, not to re-verify
// button-click semantics (already covered by the scripted tests above).
void closeNextModal()
{
    QTimer::singleShot(0, []()
                       {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close(); });
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
