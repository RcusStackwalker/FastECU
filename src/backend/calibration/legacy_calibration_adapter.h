#pragma once
#include <QString>

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

  private:
    IFileRepository& file_repository_;
};

} // namespace fastecu::calibration
