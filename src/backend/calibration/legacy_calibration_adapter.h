#pragma once
#include <QString>

#include "src/backend/definition/definition_model.h"
#include "src/backend/definitions/config_values.h"
#include "src/backend/definitions/ecu_cal_def.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

class LegacyCalibrationAdapter
{
  public:
    explicit LegacyCalibrationAdapter(IFileRepository& file_repository);

    // Reads (or backs up already-loaded) ROM bytes into ecuCalDef.FullRomData
    // and sets FileName/FullFileName. Does not touch definition matching,
    // use_romraider_definition/use_ecuflash_definition, or the
    // MapData/XScaleData/YScaleData computation (all remain
    // FileActions::open_subaru_rom_file's own inline work).
    Status open_rom_bytes(definitions::EcuCalDefStructure& ecu_cal_def, QString filename,
                          const definitions::ConfigValuesStructure& config_values);

    // Scans a fresh CarModelCatalog x ProtocolCatalog join for the row whose
    // protocol_name equals flash_method, and writes its fields into
    // config_values's nine flash_protocol_selected_* scalars. Leaves those
    // scalars untouched (not cleared) when no row matches, or when
    // protocols.cfg/car_models fail to load -- matching
    // open_subaru_rom_file's own scan, which only ever assigns inside its
    // `if (match)` branch and never clears on a miss.
    void bind_protocol(definitions::ConfigValuesStructure& config_values,
                       const QString& flash_method);

    definitions::EcuCalDefStructure *save_subaru_rom_file(
        definitions::EcuCalDefStructure *ecu_cal_def, const QString& filename);

    // Applies calibration_service.h's apply_flash_method_padding to
    // ecu_cal_def.FullRomData IN PLACE (the sub_ecu_denso_mc68hc16y5_02
    // special case; a no-op for every other flash method and for ROMs at
    // or above its size threshold). Must run before validate_rom_size and
    // before compute_map_cell_values: WRX02 definitions address
    // calibration data that only lands in bounds once the ROM has grown by
    // 0x8000 bytes, so validating the unpadded length rejects the whole
    // definition. Padding the caller's stored bytes -- rather than a
    // throwaway copy -- is also what keeps map display, map edits,
    // checksum correction, save-to-file and the flash writer all agreeing
    // on one set of addresses.
    void apply_flash_method_padding(definitions::EcuCalDefStructure& ecu_cal_def,
                                    const QString& flash_method);

    // Calls compute_map_cell_values on ecu_cal_def.FullRomData (already
    // padded by apply_flash_method_padding above) and
    // writes the resulting MapCellValuesList into ecu_cal_def.MapData/
    // XScaleData/YScaleData by position -- matching ecu_cal_def.NameList's
    // existing order, which is guaranteed (not merely assumed) to equal
    // rom_definition.maps's order: both are populated from the same
    // DefinitionService::load(catalog, format, id) call FileActions
    // already makes (once for the definition-matching dispatch that sets
    // NameList, once more for validate_rom_size that produces
    // rom_definition) -- a pure function of identical inputs, so both
    // calls yield byte-identical RomDefinition objects. A map whose decode
    // failed is logged (by name and index) and its three entries are left
    // unchanged -- only that map's, not the whole ROM's.
    void compute_map_cell_values(definitions::EcuCalDefStructure& ecu_cal_def,
                                 const definition::RomDefinition& rom_definition,
                                 const QString& flash_method,
                                 int float_precision);

  private:
    IFileRepository& file_repository_;
};

} // namespace fastecu::calibration
