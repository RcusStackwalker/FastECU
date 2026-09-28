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

QStringList rom_info_labels()
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

QString rom_info_value(const QStringList& values, RomInfoRow row)
{
    return values.value(static_cast<int>(row));
}

QStringList rom_info_values(const calibration::CalibrationSession& session,
                            const std::optional<QString>& placeholder_make)
{
    const calibration::RomProtocolInfo& protocol = session.protocol();
    QStringList values(kRomInfoRowCount, QString(" "));
    if (const calibration::ResolvedDefinition *resolved = session.definition(); resolved != nullptr)
    {
        // populate_rom_info starts from empty strings, not the " " pre-fill.
        values = QStringList(kRomInfoRowCount, QString{});
        const definition::RomDefinition& definition = resolved->definition;
        set(values, RomInfoRow::XmlId, qs(definition.identity.xml_id));
        set(values, RomInfoRow::InternalIdAddress,
            definition.identity.internal_id_address.has_value()
                ? qs(std::format("{:x}", *definition.identity.internal_id_address)) // "0x" stripped
                : QString{});
        set(values, RomInfoRow::InternalIdString, qs(definition.identity.internal_id));
        set(values, RomInfoRow::EcuId, qs(definition.identity.ecu_id));
        set(values, RomInfoRow::Make, qs(definition.metadata.make));
        set(values, RomInfoRow::Market, qs(definition.metadata.market));
        set(values, RomInfoRow::Model, qs(definition.metadata.model));
        set(values, RomInfoRow::SubModel, qs(definition.metadata.submodel));
        set(values, RomInfoRow::Transmission, qs(definition.metadata.transmission));
        set(values, RomInfoRow::Year, qs(definition.metadata.year));
        set(values, RomInfoRow::FlashMethod, qs(protocol.flash_method));
        set(values, RomInfoRow::MemModel, qs(definition.metadata.memory_model));
        set(values, RomInfoRow::ChecksumModule, qs(definition.metadata.checksum_module));
        set(values, RomInfoRow::RomBase, definition.parents.empty() ? QString{} : qs(definition.parents.front()));
        set(values, RomInfoRow::DefFile, qs(definition.source));
    }
    else if (!protocol.flash_method.empty())
    {
        set(values, RomInfoRow::FlashMethod, qs(protocol.flash_method));
    }
    if (!protocol.checksum_module.empty())
    {
        set(values, RomInfoRow::ChecksumModule, qs(protocol.checksum_module));
    }
    set(values, RomInfoRow::FileSize, qs(protocol.file_size_label));
    if (placeholder_make.has_value())
    {
        set(values, RomInfoRow::XmlId, "UnknownID");
        set(values, RomInfoRow::InternalIdAddress, "");
        set(values, RomInfoRow::InternalIdString, "");
        set(values, RomInfoRow::EcuId, "");
        set(values, RomInfoRow::Make, *placeholder_make);
        set(values, RomInfoRow::DefFile, " ");
    }
    return values;
}

} // namespace fastecu::ui
