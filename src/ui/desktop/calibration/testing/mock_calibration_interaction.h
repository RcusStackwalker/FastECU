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
    MOCK_METHOD(bool, confirmWriteWithoutChecksum, (), (override));
    MOCK_METHOD(ChecksumCorrectionResult, correctChecksums,
                (bytes::ByteView image, bool hasDefinition, const checksum::ChecksumSelection& selection), (override));
    MOCK_METHOD(std::optional<std::string>, chooseSavePath, (std::string_view suggestedPath), (override));
    MOCK_METHOD(void, showNotice, (CalibrationNotice notice), (override));
};

} // namespace fastecu::ui
