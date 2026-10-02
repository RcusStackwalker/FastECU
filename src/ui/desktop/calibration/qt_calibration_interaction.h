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

    bool confirm_write_without_checksum() override;
    ChecksumCorrectionResult correct_checksums(bytes::ByteView image, bool has_definition,
                                               const checksum::ChecksumSelection& selection) override;
    std::optional<std::string> choose_save_path(std::string_view suggested_path) override;
    void show_notice(CalibrationNotice notice) override;

  private:
    QWidget *parent_;
    ChecksumCorrectionCommand checksum_command_;
};

} // namespace fastecu::ui
