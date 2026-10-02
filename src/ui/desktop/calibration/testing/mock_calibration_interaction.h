#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <gmock/gmock.h>

#include "src/ui/desktop/calibration/calibration_interaction.h"

namespace fastecu::ui
{

// No default actions or catch-all expectations: wrap in StrictMock so any
// unscripted dialog fails the test.
class MockCalibrationInteraction : public ICalibrationInteraction
{
  public:
    MOCK_METHOD(bool, confirm_write_without_checksum, (), (override));
    MOCK_METHOD(ChecksumCorrectionResult, correct_checksums,
                (bytes::ByteView image, bool has_definition, const checksum::ChecksumSelection& selection), (override));
    MOCK_METHOD(std::optional<std::string>, choose_save_path, (std::string_view suggested_path), (override));
    MOCK_METHOD(void, show_notice, (CalibrationNotice notice), (override));
};

} // namespace fastecu::ui
