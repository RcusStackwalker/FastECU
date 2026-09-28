#include "src/ui/desktop/calibration/legacy_calibration_view.h"

#include <cstddef>
#include <utility>

#include "src/backend/definition/legacy/legacy_definition_adapter.h"
#include "src/ui/desktop/calibration/rom_info.h"

namespace fastecu::ui
{
namespace
{

QString qs(const std::string& text)
{
    return QString::fromStdString(text);
}

// FileActions::normalize_definition_addresses.
void strip_hex_prefix(QString& address)
{
    if (address.startsWith("0x"))
    {
        address.remove(0, 2);
    }
}

} // namespace

void refresh_legacy_metadata(const calibration::CalibrationSession& session, FileActions::EcuCalDefStructure& legacy)
{
    const QStringList values = rom_info_values(session);
    for (const RomInfoRow row : {RomInfoRow::FlashMethod, RomInfoRow::ChecksumModule, RomInfoRow::FileSize})
    {
        legacy.RomInfo[static_cast<int>(row)] = rom_info_value(values, row);
    }
    const calibration::RomProtocolInfo& protocol = session.protocol();
    legacy.RomId = qs(protocol.rom_id);
    legacy.McuType = qs(protocol.mcu_type);
    legacy.Kernel = qs(protocol.kernel_path);
    legacy.KernelStartAddr = qs(protocol.kernel_start_address);
}

Result<LegacyCalibrationView> project_legacy_calibration(const calibration::CalibrationSession& session)
{
    auto view = std::make_unique<FileActions::EcuCalDefStructure>();
    FileActions::EcuCalDefStructure& legacy = *view;
    const calibration::RomProtocolInfo& protocol = session.protocol();
    if (const calibration::ResolvedDefinition *definition = session.definition(); definition != nullptr)
    {
        if (Status projected = definition::LegacyDefinitionAdapter::project_definition(legacy, definition->definition,
                                                                                       definition->format);
            !projected.has_value())
        {
            return std::unexpected(projected.error());
        }
        for (QStringList *addresses : {&legacy.AddressList, &legacy.XScaleAddressList, &legacy.YScaleAddressList})
        {
            for (QString& address : *addresses)
            {
                strip_hex_prefix(address);
            }
        }
    }
    legacy.RomInfo = rom_info_values(session);
    legacy.FileSize = QString::number(static_cast<qulonglong>(protocol.unpadded_size));
    refresh_legacy_metadata(session, legacy);
    if (session.source().origin == calibration::RomOrigin::EcuRead)
    {
        legacy.FlashMethod = qs(protocol.flash_method);
    }
    legacy.FileName = qs(session.source().display_name);
    legacy.FullFileName = qs(session.source().path);
    legacy.FullRomData =
        QByteArray(reinterpret_cast<const char *>(session.rom().data()), static_cast<qsizetype>(session.rom().size()));
    legacy.OemEcuFile = true;

    LegacyCalibrationView result{.view = std::move(view)};
    for (qsizetype index = 0; index < legacy.MapData.size(); ++index)
    {
        auto decoded = session.decode_map(static_cast<std::size_t>(index));
        if (!decoded.has_value())
        {
            if (!result.decode_error.has_value())
            {
                result.decode_error = decoded.error();
            }
            continue;
        }
        legacy.MapData.replace(index, qs(decoded->map_data));
        legacy.XScaleData.replace(index, qs(decoded->x_axis_data));
        legacy.YScaleData.replace(index, qs(decoded->y_axis_data));
    }
    return result;
}

} // namespace fastecu::ui
