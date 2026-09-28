#pragma once
#include <string_view>

#include <QString>

#include "src/backend/definition/definition_model.h"
#include "src/backend/definitions/ecu_cal_def.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

class LegacyCalibrationAdapter
{
  public:
    explicit LegacyCalibrationAdapter(IFileRepository& file_repository);

    // Two modes, selected by whether ecuCalDef.FullRomData already holds an
    // image:
    //   * empty -- reads `filename` into FullRomData (read_rom). An empty
    //     `filename` here is an error, since there is nothing to open.
    //   * non-empty -- the caller already has the image (e.g. straight off
    //     the ECU); backs it up to <calibration_files_directory>/read.bin
    //     (backup_rom) and leaves FullRomData strictly alone. A generated
    //     "read_image_<timestamp>.bin" stands in for an empty `filename`.
    // Either way sets FileName/FullFileName. Does not touch definition
    // matching, use_romraider_definition/use_ecuflash_definition, or the later
    // MapData/XScaleData/YScaleData computation.
    Status open_rom_bytes(definitions::EcuCalDefStructure& ecu_cal_def, QString filename,
                          std::string_view calibration_files_directory);

    // Pads ecu_cal_def.FullRomData in place for the WRX02 family. Must be
    // called BEFORE validate_rom_size: padding grows the image by 0x8000
    // bytes, and definitions for that family address the padded layout, so a
    // size check against the unpadded length rejects a fine ROM.
    //
    // Writes back into FullRomData rather than returning a copy -- every later
    // consumer, including the flash write path, reads that same buffer.
    void apply_flash_method_padding(definitions::EcuCalDefStructure& ecu_cal_def, const QString& flash_method);

    // Decodes every map's cells and axes from FullRomData and writes them into
    // the legacy MapData/XScaleData/YScaleData columns, one entry per map in
    // definition order. A map whose decode fails is skipped -- its columns keep
    // whatever they held -- and reported through the returned status's detail;
    // sibling maps are still written. Returns a failure if any map decode fails,
    // or if the definition and legacy columns disagree on length (which means
    // the definition adapter and this call saw different definitions).
    Status compute_map_cell_values(definitions::EcuCalDefStructure& ecu_cal_def,
                                   const definition::RomDefinition& rom_definition);

    definitions::EcuCalDefStructure *save_subaru_rom_file(definitions::EcuCalDefStructure *ecu_cal_def,
                                                          const QString& filename);

  private:
    IFileRepository& file_repository_;
};

} // namespace fastecu::calibration
