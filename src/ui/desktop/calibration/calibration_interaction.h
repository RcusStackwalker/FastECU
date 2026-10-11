#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "src/algorithms/memory/memory_image.h"
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
    virtual bool confirmWriteWithoutChecksum() = 0;
    // `image` is the operation's ROM file placed by the selected protocol's
    // memory map; corrected_rom_data, when present, is that ROM file corrected.
    virtual ChecksumCorrectionResult correctChecksums(const memory::MemoryImage& image, bool hasDefinition,
                                                      const checksum::ChecksumSelection& selection) = 0;
    // nullopt when the user dismissed the picker.
    virtual std::optional<std::string> chooseSavePath(std::string_view suggestedPath) = 0;
    virtual void showNotice(CalibrationNotice notice) = 0;
};

} // namespace fastecu::ui
