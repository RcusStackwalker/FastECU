#pragma once

#include <optional>

#include <QString>
#include <QStringList>

#include "src/backend/calibration/session/calibration_session.h"

namespace fastecu::ui
{

// The "ROM Info" rows of the calibration data tree, in legacy order
// (EcuCalDefStructure::RomInfoStrings / FileActions::RomInfoEnum).
enum class RomInfoRow : int
{
    XmlId,
    InternalIdAddress,
    InternalIdString,
    EcuId,
    Make,
    Market,
    Model,
    SubModel,
    Transmission,
    Year,
    FlashMethod,
    MemModel,
    ChecksumModule,
    RomBase,
    FileSize,
    DefFile,
};
inline constexpr int kRomInfoRowCount = 16;

QStringList rom_info_labels();
QString rom_info_value(const QStringList& values, RomInfoRow row);

// The 16 ROM Info values legacy displayed: MainWindow's " " pre-fill, the
// definition's identity and metadata (FileActions::open_subaru_rom_file after
// normalize_definition_addresses), then the session's protocol info. A set
// placeholder_make applies FileActions::apply_missing_definition_defaults.
QStringList rom_info_values(const calibration::CalibrationSession& session,
                            const std::optional<QString>& placeholder_make = std::nullopt);

} // namespace fastecu::ui
