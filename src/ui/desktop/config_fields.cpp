#include "src/ui/desktop/config_fields.h"

namespace fastecu::ui
{

QString protocol_field(const config::VehicleSpec& vehicle, std::string_view config::ProtocolSpec::*field)
{
    return qs(vehicle.protocol->*field);
}

QString protocol_flag(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability)
{
    return protocol_capability(vehicle, capability) ? QStringLiteral("yes") : QStringLiteral("no");
}

bool protocol_capability(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability)
{
    return vehicle.protocol->*capability;
}

QString checksum_field(const config::VehicleSpec& vehicle)
{
    return qs(config::checksum_flag(vehicle.protocol->checksum));
}

QString kernel_address_field(const config::VehicleSpec& vehicle)
{
    return QString::fromStdString(config::kernel_load_address_text(*vehicle.protocol));
}

QStringList qstring_list(const std::vector<std::string>& items)
{
    QStringList out;
    out.reserve(static_cast<qsizetype>(items.size()));
    for (const std::string& item : items)
    {
        out.append(qs(item));
    }
    return out;
}

std::vector<std::string> string_vector(const QStringList& items)
{
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(items.size()));
    for (const QString& item : items)
    {
        out.push_back(item.toStdString());
    }
    return out;
}

} // namespace fastecu::ui
