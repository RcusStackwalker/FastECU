#pragma once

#include <optional>

#include "src/algorithms/checksum/checksum_result.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/checksum/checksum_selection.h"
#include "src/ui/desktop/checksum/checksum_correction_result.h"

class QWidget;

namespace fastecu::ui
{

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

    ChecksumCorrectionResult run(bytes::ByteView romData, bool hasDefinition,
                                 const fastecu::checksum::ChecksumSelection& selection, QWidget *parent);

  protected:
    // Returns true if the user chose to proceed anyway ("DO IT!"), false for
    // the default "OK" (abort).
    virtual bool confirmProceedWithoutDefinition(QWidget *parent);

    virtual void showBadRomSizeDialog(QWidget *parent);

    // Returns true if the user chose Cancel (abort correction), false for OK.
    virtual bool confirmProceedWithoutChecksumModule();

    virtual void showFamilyResultDialog(const ChecksumResult& familyResult);
};

} // namespace fastecu::ui
