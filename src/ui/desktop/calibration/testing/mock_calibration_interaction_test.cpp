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

    ChecksumCorrectionResult correctedResult;
    correctedResult.corrected_rom_data = bytes::Bytes{4, 5, 6};
    EXPECT_CALL(mock, confirmWriteWithoutChecksum()).WillOnce(Return(false));
    EXPECT_CALL(mock, correctChecksums(_, true, _)).WillOnce(Return(correctedResult));
    EXPECT_CALL(mock, chooseSavePath(std::string_view("/old/read.bin")))
        .WillOnce(Return(std::optional<std::string>("/cal/new.bin")));
    EXPECT_CALL(mock, showNotice(CalibrationNotice::kNoSaveFilename)).Times(1);

    EXPECT_FALSE(port.confirmWriteWithoutChecksum());
    const auto corrected = port.correctChecksums(image, true, selection);
    ASSERT_TRUE(corrected.corrected_rom_data.has_value());
    EXPECT_THAT(*corrected.corrected_rom_data, ElementsAre(4, 5, 6));
    EXPECT_EQ(port.chooseSavePath("/old/read.bin"), "/cal/new.bin");
    port.showNotice(CalibrationNotice::kNoSaveFilename);
}
