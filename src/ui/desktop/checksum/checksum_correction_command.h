#pragma once

#include <optional>

#include "src/algorithms/checksum/checksum_result.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/checksum/checksum_selection.h"

class QWidget;

namespace fastecu::ui
{

struct ChecksumCorrectionResult
{
    std::optional<bytes::Bytes> corrected_rom_data;
    bool canceled_due_to_missing_module = false;
    bool unknown_mcu_type = false;
};

// Owns the checksum-correction dialog sequence, hoisted out of
// FileActions::checksum_correction and the backend LegacyChecksumAdapter in
// step 5e so that no QMessageBox is raised from src/backend.
//
// The protected virtual seams let a test subclass script answers without
// showing a real modal QMessageBox -- same pattern the deleted adapter used.
class ChecksumCorrectionCommand
{
  public:
    virtual ~ChecksumCorrectionCommand() = default;

    ChecksumCorrectionResult run(bytes::ByteView rom_data, bool use_romraider_definition, bool use_ecuflash_definition,
                                 const fastecu::checksum::ChecksumSelection& selection, QWidget *parent);

  protected:
    // Returns true if the user chose to proceed anyway ("DO IT!"), false for
    // the default "OK" (abort).
    virtual bool confirmProceedWithoutDefinition(QWidget *parent);

    virtual void showBadRomSizeDialog(QWidget *parent);

    // Returns true if the user chose Cancel (abort correction), false for OK.
    virtual bool confirmProceedWithoutChecksumModule();

    virtual void showFamilyResultDialog(const ChecksumResult& family_result);
};

} // namespace fastecu::ui
