#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "src/ui/desktop/calibration/calibration_interaction.h"
#include "src/ui/desktop/checksum/checksum_correction_command.h"

class QWidget;

namespace fastecu::ui
{

// Qt dialogs behind ICalibrationInteraction. Borrows parent; owns the checksum
// command.
class QtCalibrationInteraction final : public ICalibrationInteraction
{
  public:
    explicit QtCalibrationInteraction(QWidget *parent);

    bool confirmWriteWithoutChecksum() override;
    ChecksumCorrectionResult correctChecksums(const memory::MemoryImage& image, bool hasDefinition,
                                              const checksum::ChecksumSelection& selection) override;
    std::optional<std::string> chooseSavePath(std::string_view suggestedPath) override;
    void showNotice(CalibrationNotice notice) override;

  private:
    QWidget *parent_;
    ChecksumCorrectionCommand checksum_command_;
};

} // namespace fastecu::ui
