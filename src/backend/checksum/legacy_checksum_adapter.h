#pragma once
#include <optional>
#include "src/algorithms/checksum/checksum_result.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/checksum/checksum_selection.h"

class QWidget;

namespace fastecu::checksum
{

struct LegacyChecksumAdapterResult
{
    std::optional<bytes::Bytes> corrected_rom_data;
    bool canceled_due_to_missing_module = false;
};

// Bridges apply_checksum_correction (portable) back into the QMessageBox
// dialogs and ROM-byte mutation FileActions::checksum_correction performs
// today. Protected virtual dialog seams let a test subclass script answers
// without showing a real modal QMessageBox -- same pattern as
// EepromEcuSubaruDensoSH705xKline's confirm()/showFailureDialog() seams
// (src/ui/desktop/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_kline.h).
//
// Takes primitive/portable parameters rather than FileActions::EcuCalDefStructure&
// to avoid a Bazel dependency cycle -- see this class's entry in
// docs/superpowers/plans/2026-07-25-step5d2-checksum-use-case.md, Task 3, for
// the full reasoning (same shape as legacy_config_adapter.h's
// ConfigValuesStructure extraction, applied by avoiding the struct
// dependency instead of extracting EcuCalDefStructure).
class LegacyChecksumAdapter
{
  public:
    virtual ~LegacyChecksumAdapter() = default;

    LegacyChecksumAdapterResult checksum_correction(bytes::ByteView rom_data, bool use_romraider_definition,
                                                    bool use_ecuflash_definition, const ChecksumSelection& selection,
                                                    QWidget *parent);

  protected:
    // Returns true if the user chose to proceed anyway ("DO IT!"), false for
    // the default "OK" (abort). Real implementation: the WARNING/OK/"DO IT!"
    // QMessageBox from checksum_correction (file_actions.cpp:2167-2179).
    virtual bool confirmProceedWithoutDefinition(QWidget *parent);

    // Real implementation: the "Bad ROM size!" information dialog
    // (file_actions.cpp:2193).
    virtual void showBadRomSizeDialog(QWidget *parent);

    // Returns true if the user chose Cancel (abort correction), false for OK
    // (do nothing further -- there is no module to run anyway). Real
    // implementation: the WARNING/Cancel/OK QMessageBox
    // (file_actions.cpp:2349-2357).
    virtual bool confirmProceedWithoutChecksumModule();

    // Real implementation: showChecksumResult's Disabled info dialog / the
    // aggregated "Checksums corrected" info dialog / the InvalidSize,
    // UnsupportedRom, ParseError warning dialog (file_actions.cpp:61-77,
    // 2339-2345), selected by family_result.status.
    virtual void showFamilyResultDialog(const ChecksumResult& family_result);
};

} // namespace fastecu::checksum
