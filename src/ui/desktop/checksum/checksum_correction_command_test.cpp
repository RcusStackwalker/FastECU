#include "src/ui/desktop/checksum/checksum_correction_command.h"

#include <QWidget>
#include <gtest/gtest.h>

#include <array>

#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/memory/testing/memory_views.h"

// ChecksumResult is in the global namespace -- see the namespace note above.
using fastecu::checksum::ChecksumSelection;
using fastecu::ui::ChecksumCorrectionCommand;
using fastecu::ui::ChecksumCorrectionResult;

namespace
{
namespace memory = fastecu::memory;

memory::MemoryImage ImageOf(const bytes::Bytes& rom)
{
    return memory::testing::ImageAt(memory::FlashAddress{0}, rom);
}

ChecksumSelection subaruSh72543rSelection()
{
    ChecksumSelection selection;
    selection.make = "Subaru";
    selection.checksum_flag = "yes";
    selection.flash_method = "sub_ecu_hitachi_sh72543r";
    selection.mcu_type = "SH72543R";
    selection.rom_id = "39670016";
    return selection;
}

class TestableChecksumCommand : public ChecksumCorrectionCommand
{
  public:
    bool proceed_without_definition_answer = true; // "DO IT!" by default
    bool cancel_without_module_answer = false;     // "OK" (proceed) by default
    int missing_definition_dialog_count = 0;
    int bad_rom_size_dialog_count = 0;
    int family_result_dialog_count = 0;
    ChecksumResult last_family_result;

  protected:
    bool confirmProceedWithoutDefinition(QWidget *) override
    {
        ++missing_definition_dialog_count;
        return proceed_without_definition_answer;
    }
    void showBadRomSizeDialog(QWidget *) override
    {
        ++bad_rom_size_dialog_count;
    }
    bool confirmProceedWithoutChecksumModule() override
    {
        return cancel_without_module_answer;
    }
    void showFamilyResultDialog(const ChecksumResult& familyResult) override
    {
        ++family_result_dialog_count;
        last_family_result = familyResult;
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
    command.proceed_without_definition_answer = false;
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(ImageOf(rom), false, subaruM32rKlineSelection(), nullptr);

    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_FALSE(result.canceled_due_to_missing_module);
    EXPECT_EQ(command.family_result_dialog_count, 0);
}

TEST(ChecksumCorrectionCommand, AcceptingGateWithoutLinkedDefinitionCorrectsRom)
{
    TestableChecksumCommand command;
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(ImageOf(rom), false, subaruM32rKlineSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(command.family_result_dialog_count, 1);
    EXPECT_EQ(command.last_family_result.status, ChecksumResult::Status::kCorrected);
    EXPECT_EQ(command.last_family_result.message, "Subaru Hitachi M32R K-Line ECU Checksum");
}

TEST(ChecksumCorrectionCommand, GateNotConsultedWhenDefinitionAlreadyLinked)
{
    TestableChecksumCommand command;
    command.proceed_without_definition_answer = false; // would abort if the gate were (wrongly) shown
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(ImageOf(rom), true, subaruM32rKlineSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
}

TEST(ChecksumCorrectionCommand, HeaderOnlyDefinitionBypassesMissingDefinitionGate)
{
    TestableChecksumCommand command;
    command.proceed_without_definition_answer = false;
    const bytes::Bytes rom(524288, 0);
    // Definition presence is independent of map count: a header-only definition
    // supplies checksum selection metadata and still links this image.
    constexpr bool kHasDefinition = true;

    const auto result = command.run(ImageOf(rom), kHasDefinition, subaruM32rKlineSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(command.missing_definition_dialog_count, 0);
    EXPECT_EQ(command.family_result_dialog_count, 1);
}

TEST(ChecksumCorrectionCommand, DisabledDieselChecksumPreservesRomData)
{
    TestableChecksumCommand command;
    bytes::Bytes rom(1024UZ * 1024, 0);
    bytes::WriteU32Be(rom, 0x0FFB88, 0x5AA5A55A);

    const ChecksumCorrectionResult result =
        command.run(ImageOf(rom), true, subaruDensoSh7058DieselSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(*result.corrected_rom_data, rom);
    EXPECT_EQ(command.last_family_result.status, ChecksumResult::Status::kDisabled);
}

TEST(ChecksumCorrectionCommand, BadRomSizeShowsDialogAndMakesNoCorrection)
{
    TestableChecksumCommand command;
    const bytes::Bytes rom(4096, 0); // wrong size for M32R_512KB

    ChecksumCorrectionResult result = command.run(ImageOf(rom), true, subaruM32rKlineSelection(), nullptr);

    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_EQ(command.bad_rom_size_dialog_count, 1);
}

TEST(ChecksumCorrectionCommand, NoModuleWithChecksumFlagNoAsksNothingAndDoesNotCancel)
{
    TestableChecksumCommand command;
    ChecksumSelection selection = subaruM32rKlineSelection();
    selection.checksum_flag = "no";
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(ImageOf(rom), true, selection, nullptr);

    EXPECT_FALSE(result.canceled_due_to_missing_module);
}

TEST(ChecksumCorrectionCommand, NoModuleWithChecksumFlagNaAsksAndRespectsCancel)
{
    TestableChecksumCommand command;
    command.cancel_without_module_answer = true;
    ChecksumSelection selection = subaruM32rKlineSelection();
    selection.checksum_flag = "n/a";
    const bytes::Bytes rom(524288, 0);

    ChecksumCorrectionResult result = command.run(ImageOf(rom), true, selection, nullptr);

    EXPECT_TRUE(result.canceled_due_to_missing_module);
}

TEST(ChecksumCorrectionCommand, UnknownMcuTypeReturnsUnmodifiedRomAndRunsNoDialog)
{
    // "M32170" is MUT/DMA logging's MCU in the built-in catalog; it offers
    // no flash operation and has no kFlashDevices[] entry. Formerly checksum_correction's early return.
    TestableChecksumCommand command;
    ChecksumSelection selection = subaruM32rKlineSelection();
    selection.mcu_type = "M32170";

    const bytes::Bytes rom(100, bytes::Byte{0});
    const ChecksumCorrectionResult result = command.run(ImageOf(rom), true, selection, nullptr);

    EXPECT_TRUE(result.unknown_mcu_type);
    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_EQ(command.bad_rom_size_dialog_count, 0);
    EXPECT_EQ(command.family_result_dialog_count, 0);
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
    const bytes::Bytes original = rom;
    const ChecksumCorrectionResult result = command.run(ImageOf(rom), true, selection, nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(result.corrected_rom_data->size(), 524288U);
    EXPECT_NE(*result.corrected_rom_data, rom);
    EXPECT_EQ(rom, original);
    EXPECT_EQ(command.family_result_dialog_count, 1);
}

// SH72543R rebalances the word at ECU 0x1FFFFE; with the two 1 MiB halves in
// swapped file order the operation image gets it at file offset 0x0FFFFE.
TEST(ChecksumCorrectionCommand, CorrectedBytesAreTheRomFileNotTheEcuRange)
{
    TestableChecksumCommand command;
    const auto range = [](std::uint32_t start)
    {
        return memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{0x100000})
            .value();
    };
    const std::array blocks{
        memory::MemoryBlock{.range = range(0),
                            .backing = memory::FileBacking{.offset = memory::FileOffset{0x100000}},
                            .writability = memory::Writability::kWritable},
        memory::MemoryBlock{.range = range(0x100000),
                            .backing = memory::FileBacking{.offset = memory::FileOffset{0}},
                            .writability = memory::Writability::kWritable},
    };
    const auto image = memory::MemoryImage::Create(
        memory::MemoryMap::Create(blocks, memory::ByteCount{0x200000}).value(), bytes::Bytes(0x200000, 0));
    ASSERT_TRUE(image.has_value());

    const ChecksumCorrectionResult result = command.run(*image, true, subaruSh72543rSelection(), nullptr);

    ASSERT_TRUE(result.corrected_rom_data.has_value());
    EXPECT_EQ(bytes::ReadU16Be(*result.corrected_rom_data, 0x0FFFFE), 0x5AA5U);
    EXPECT_EQ(bytes::ReadU16Be(*result.corrected_rom_data, 0x1FFFFE), 0U);
}

TEST(ChecksumCorrectionCommand, ACorrectionIntoReadOnlyMemoryChangesNothingAndWarns)
{
    TestableChecksumCommand command;
    const memory::MemoryImage image =
        memory::testing::ImageAt(memory::FlashAddress{0}, bytes::Bytes(0x200000, 0), memory::Writability::kReadOnly);

    const ChecksumCorrectionResult result = command.run(image, true, subaruSh72543rSelection(), nullptr);

    EXPECT_FALSE(result.corrected_rom_data.has_value());
    EXPECT_EQ(command.family_result_dialog_count, 1);
    EXPECT_EQ(command.last_family_result.status, ChecksumResult::Status::kUnsupportedRom);
}
