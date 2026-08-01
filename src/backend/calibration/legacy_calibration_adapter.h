#pragma once
#include <optional>
#include <string>
#include <vector>

#include <QString>

#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/config_paths.h"
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
                          const definitions::ConfigValuesStructure& config_values);

    // Scans the CarModelCatalog x ProtocolCatalog join for the row whose
    // protocol_name equals flash_method, and writes its fields into
    // config_values's nine flash_protocol_selected_* scalars. This is the
    // sole writer of those scalars on the ROM-open path -- it replaces
    // open_subaru_rom_file's own scan over the parallel flash_protocol_*
    // QStringLists, so it must reproduce that scan's observable behavior
    // exactly:
    //   * no matching row (or protocols.cfg failing to load): all nine
    //     scalars left untouched, not cleared -- the legacy loop only ever
    //     assigned inside its `if (match)` branch.
    //   * a matching row whose protocol_name matched no <protocol>: the four
    //     protocol-derived scalars (description/log_protocol/mcu/checksum)
    //     get the legacy placeholder, a single space, because that is what
    //     LegacyConfigAdapter::copy_car_models_into_legacy put in the
    //     parallel lists for such a row. They must NOT keep whatever the
    //     previously opened ROM left in them.
    void bind_protocol(definitions::ConfigValuesStructure& config_values,
                       const QString& flash_method);

    // Pads ecu_cal_def.FullRomData in place for the WRX02 family. Must be
    // called BEFORE validate_rom_size: padding grows the image by 0x8000
    // bytes, and definitions for that family address the padded layout, so a
    // size check against the unpadded length rejects a fine ROM.
    //
    // Writes back into FullRomData rather than returning a copy -- every later
    // consumer, including the flash write path, reads that same buffer.
    void apply_flash_method_padding(definitions::EcuCalDefStructure& ecu_cal_def,
                                    const QString& flash_method);

    // Decodes every map's cells and axes from FullRomData and writes them into
    // the legacy MapData/XScaleData/YScaleData columns, one entry per map in
    // definition order. A map whose decode fails is skipped -- its columns keep
    // whatever they held -- and reported through the returned status's detail;
    // sibling maps are still written. Returns a failure if any map decode fails,
    // or if the definition and legacy columns disagree on length (which means
    // the definition adapter and this call saw different definitions).
    Status compute_map_cell_values(definitions::EcuCalDefStructure& ecu_cal_def,
                                   const definition::RomDefinition& rom_definition);

    definitions::EcuCalDefStructure *save_subaru_rom_file(
        definitions::EcuCalDefStructure *ecu_cal_def, const QString& filename);

  private:
    // Resolved join for `paths`, or nullptr if either catalog failed to
    // load. Memoized: bind_protocol runs on every ROM open, and both
    // load_protocol_catalog and load_car_model_catalog read and re-parse the
    // same paths.protocols_file, so an uncached call costs two full reads
    // plus two XML parses of a file that essentially never changes. Legacy
    // parsed those lists once at startup (read_protocols_file), so caching
    // is also the closer match to pre-refactor behavior.
    const std::vector<config::ResolvedCarModel> *resolved_car_models(
        const config::ConfigPaths& paths);

    IFileRepository& file_repository_;
    // Cache plus the protocols_file handle it was built from; a different
    // handle rebuilds. A failed load is not cached, so a later successful
    // load still populates it.
    std::optional<std::vector<config::ResolvedCarModel>> resolved_car_models_cache_;
    std::string resolved_car_models_handle_;
};

} // namespace fastecu::calibration
