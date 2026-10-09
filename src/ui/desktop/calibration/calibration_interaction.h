#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/checksum/checksum_selection.h"
#include "src/ui/desktop/checksum/checksum_correction_result.h"

namespace fastecu::ui
{

enum class CalibrationNotice
{
    kNoCalibrationToWrite,
    kNoCalibrationToSave,
    kNoSaveFilename
};

// The dialogs a calibration operation raises, behind a UI-owned port so the
// operation logic can be driven without a real modal dialog.
class ICalibrationInteraction
{
  public:
    virtual ~ICalibrationInteraction() = default;

    // True to proceed with the write despite the missing checksum module.
    virtual bool confirm_write_without_checksum() = 0;
    virtual ChecksumCorrectionResult correct_checksums(bytes::ByteView image, bool has_definition,
                                                       const checksum::ChecksumSelection& selection) = 0;
    // nullopt when the user dismissed the picker.
    virtual std::optional<std::string> choose_save_path(std::string_view suggested_path) = 0;
    virtual void show_notice(CalibrationNotice notice) = 0;
};

} // namespace fastecu::ui
