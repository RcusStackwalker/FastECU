#include "src/ui/desktop/calibration/testing/mock_calibration_interaction.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using fastecu::checksum::ChecksumSelection;
using fastecu::ui::CalibrationNotice;
using fastecu::ui::ChecksumCorrectionResult;
using fastecu::ui::ICalibrationInteraction;
using fastecu::ui::MockCalibrationInteraction;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;
using ::testing::StrictMock;

TEST(MockCalibrationInteraction, DispatchesScriptedRepliesThroughInterface)
{
    StrictMock<MockCalibrationInteraction> mock;
    ICalibrationInteraction& port = mock;
    const bytes::Bytes image{1, 2, 3};
    const ChecksumSelection selection;

    ChecksumCorrectionResult corrected_result;
    corrected_result.corrected_rom_data = bytes::Bytes{4, 5, 6};
    EXPECT_CALL(mock, confirm_write_without_checksum()).WillOnce(Return(false));
    EXPECT_CALL(mock, correct_checksums(_, true, _)).WillOnce(Return(corrected_result));
    EXPECT_CALL(mock, choose_save_path(std::string_view("/old/read.bin")))
        .WillOnce(Return(std::optional<std::string>("/cal/new.bin")));
    EXPECT_CALL(mock, show_notice(CalibrationNotice::NoSaveFilename)).Times(1);

    EXPECT_FALSE(port.confirm_write_without_checksum());
    const auto corrected = port.correct_checksums(image, true, selection);
    ASSERT_TRUE(corrected.corrected_rom_data.has_value());
    EXPECT_THAT(*corrected.corrected_rom_data, ElementsAre(4, 5, 6));
    EXPECT_EQ(port.choose_save_path("/old/read.bin"), "/cal/new.bin");
    port.show_notice(CalibrationNotice::NoSaveFilename);
}
