#pragma once

#include <QStringList>

namespace fastecu::definitions
{

// FileActions's legacy EcuFlash/RomRaider definition indexes: four parallel
// lists per format (calibration ID, its ROM address, ECU ID, and source file).
// Built from the configured definition sources and appended to by
// definition authoring. They are derived data, not application settings,
// so they live beside FileActions rather than in the configuration session.
struct DefinitionIndexes
{
    bool operator==(const DefinitionIndexes&) const = default;

    QStringList ecuflash_def_cal_id;
    QStringList ecuflash_def_cal_id_addr;
    QStringList ecuflash_def_ecu_id;
    QStringList ecuflash_def_filename;
    QStringList romraider_def_cal_id;
    QStringList romraider_def_cal_id_addr;
    QStringList romraider_def_ecu_id;
    QStringList romraider_def_filename;
};

} // namespace fastecu::definitions
