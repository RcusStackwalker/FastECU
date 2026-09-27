#include "src/ui/desktop/config_fields.h"

#include "src/backend/config/config_session.h"

namespace fastecu::ui
{

QString protocol_field(const config::ResolvedCarModel& vehicle, std::string config::ProtocolEntry::*field)
{
    return qs(config::protocol_field_or_placeholder(vehicle, field));
}

bool protocol_capability(const config::ResolvedCarModel& vehicle, std::string config::ProtocolEntry::*capability)
{
    return vehicle.protocol.has_value() && (*vehicle.protocol).*capability == "yes";
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
