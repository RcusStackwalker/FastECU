// src/platform/desktop/common/flash/flash_snapshot_adapter.h
#pragma once
#include <string>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/ports/file_repository.h"

namespace fastecu::flash
{

// Copies legacy FileActions/EcuCalDefStructure state and kernel bytes into a
// portable DensoSh705xEepromInput and calls the portable builder. Step 5d
// replaces this class with a backend definition/flash use case; the plan
// and executor contracts this adapter targets do not change.
class LegacyFlashSnapshotAdapter
{
  public:
    explicit LegacyFlashSnapshotAdapter(IFileRepository& file_repository);

    Result<FlashPlan> build_read_plan(const FileActions::EcuCalDefStructure& ecu_cal_def,
                                      const std::string& protocol_name, EepromReadMode mode,
                                      const std::string& kernel_file_handle) const;

  private:
    IFileRepository& file_repository_;
};

} // namespace fastecu::flash
