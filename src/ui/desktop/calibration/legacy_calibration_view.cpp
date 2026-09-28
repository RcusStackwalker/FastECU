#include "src/ui/desktop/calibration/legacy_calibration_view.h"

#include <cstddef>
#include <utility>

#include "src/backend/definition/legacy/legacy_definition_adapter.h"

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

Result<LegacyCalibrationView> project_legacy_calibration(const calibration::CalibrationSession& session)
{
    auto view = std::make_unique<FileActions::EcuCalDefStructure>();
    FileActions::EcuCalDefStructure& legacy = *view;
    // MainWindow's pre-fill of a fresh slot.
    while (legacy.RomInfo.length() < legacy.RomInfoStrings.length())
    {
        legacy.RomInfo.append(" ");
    }

    const calibration::RomProtocolInfo& protocol = session.protocol();
    if (const calibration::ResolvedDefinition *definition = session.definition(); definition != nullptr)
    {
        if (Status projected = definition::LegacyDefinitionAdapter::project_definition(legacy, definition->definition,
                                                                                       definition->format);
            !projected.has_value())
        {
            return std::unexpected(projected.error());
        }
        strip_hex_prefix(legacy.RomInfo[FileActions::InternalIdAddress]);
        for (QStringList *addresses : {&legacy.AddressList, &legacy.XScaleAddressList, &legacy.YScaleAddressList})
        {
            for (QString& address : *addresses)
            {
                strip_hex_prefix(address);
            }
        }
        legacy.RomInfo[FileActions::FlashMethod] = qs(protocol.flash_method);
    }
    else if (!protocol.flash_method.empty())
    {
        legacy.RomInfo[FileActions::FlashMethod] = qs(protocol.flash_method);
    }
    if (!protocol.checksum_module.empty())
    {
        legacy.RomInfo[FileActions::ChecksumModule] = qs(protocol.checksum_module);
    }
    legacy.RomInfo[FileActions::FileSize] = qs(protocol.file_size_label);
    legacy.FileSize = QString::number(static_cast<qulonglong>(protocol.unpadded_size));
    legacy.RomId = qs(protocol.rom_id);
    legacy.McuType = qs(protocol.mcu_type);
    legacy.Kernel = qs(protocol.kernel_path);
    legacy.KernelStartAddr = qs(protocol.kernel_start_address);
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
