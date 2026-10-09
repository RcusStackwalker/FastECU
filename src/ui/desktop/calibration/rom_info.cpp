#include "src/ui/desktop/calibration/rom_info.h"

#include <format>

namespace fastecu::ui
{
namespace
{

QString qs(const std::string& text)
{
    return QString::fromStdString(text);
}

void set(QStringList& values, RomInfoRow row, const QString& value)
{
    values[static_cast<int>(row)] = value;
}

} // namespace

QStringList romInfoLabels()
{
    return {
        "XML ID",
        "Internal ID Address",
        "Internal ID String",
        "ECU ID",
        "Make",
        "Market",
        "Model",
        "Submodel",
        "Transmission",
        "Year",
        "Flash Method",
        "Memory Model",
        "Checksum Module",
        "Rom Base",
        "File Size",
        "Def File",
    };
}

QString romInfoValue(const QStringList& values, RomInfoRow row)
{
    return values.value(static_cast<int>(row));
}

QStringList romInfoValues(const calibration::CalibrationSession& session, const std::optional<QString>& placeholderMake)
{
    const calibration::RomProtocolInfo& protocol = session.Protocol();
    QStringList values(kRomInfoRowCount, QString(" "));
    if (const calibration::ResolvedDefinition *resolved = session.Definition(); resolved != nullptr)
    {
        // populate_rom_info starts from empty strings, not the " " pre-fill.
        values = QStringList(kRomInfoRowCount, QString{});
        const definition::RomDefinition& definition = resolved->definition;
        set(values, RomInfoRow::kXmlId, qs(definition.identity.xml_id));
        set(values, RomInfoRow::kInternalIdAddress,
            definition.identity.internal_id_address.has_value()
                ? qs(std::format("{:x}", *definition.identity.internal_id_address)) // "0x" stripped
                : QString{});
        set(values, RomInfoRow::kInternalIdString, qs(definition.identity.internal_id));
        set(values, RomInfoRow::kEcuId, qs(definition.identity.ecu_id));
        set(values, RomInfoRow::kMake, qs(definition.metadata.make));
        set(values, RomInfoRow::kMarket, qs(definition.metadata.market));
        set(values, RomInfoRow::kModel, qs(definition.metadata.model));
        set(values, RomInfoRow::kSubModel, qs(definition.metadata.submodel));
        set(values, RomInfoRow::kTransmission, qs(definition.metadata.transmission));
        set(values, RomInfoRow::kYear, qs(definition.metadata.year));
        set(values, RomInfoRow::kFlashMethod, qs(protocol.flash_method));
        set(values, RomInfoRow::kMemModel, qs(definition.metadata.memory_model));
        set(values, RomInfoRow::kChecksumModule, qs(definition.metadata.checksum_module));
        set(values, RomInfoRow::kRomBase, definition.parents.empty() ? QString{} : qs(definition.parents.front()));
        set(values, RomInfoRow::kDefFile, qs(definition.source));
    }
    else if (!protocol.flash_method.empty())
    {
        set(values, RomInfoRow::kFlashMethod, qs(protocol.flash_method));
    }
    if (!protocol.checksum_module.empty())
    {
        set(values, RomInfoRow::kChecksumModule, qs(protocol.checksum_module));
    }
    set(values, RomInfoRow::kFileSize, qs(protocol.file_size_label));
    if (placeholderMake.has_value())
    {
        set(values, RomInfoRow::kXmlId, "UnknownID");
        set(values, RomInfoRow::kInternalIdAddress, "");
        set(values, RomInfoRow::kInternalIdString, "");
        set(values, RomInfoRow::kEcuId, "");
        set(values, RomInfoRow::kMake, *placeholderMake);
        set(values, RomInfoRow::kDefFile, " ");
    }
    return values;
}

} // namespace fastecu::ui
