#include "src/ui/desktop/config_fields.h"

#include <algorithm>
#include <iterator>
#include <ranges>

namespace fastecu::ui
{

QString protocolField(const config::VehicleSpec& vehicle, std::string_view config::ProtocolSpec::*field)
{
    return qs(vehicle.protocol->*field);
}

QString protocolFlag(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability)
{
    return protocolCapability(vehicle, capability) ? QStringLiteral("yes") : QStringLiteral("no");
}

bool protocolCapability(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability)
{
    return vehicle.protocol->*capability;
}

QString checksumField(const config::VehicleSpec& vehicle)
{
    return qs(config::ChecksumFlag(vehicle.protocol->checksum));
}

QString kernelAddressField(const config::VehicleSpec& vehicle)
{
    return QString::fromStdString(config::KernelLoadAddressText(*vehicle.protocol));
}

QStringList qstringList(const std::vector<std::string>& items)
{
    QStringList out;
    out.reserve(static_cast<qsizetype>(items.size()));
    for (const std::string& item : items)
    {
        out.append(qs(item));
    }
    return out;
}

std::vector<std::string> stringVector(const QStringList& items)
{
    return items | std::views::transform([](const QString& item) { return item.toStdString(); }) |
           std::ranges::to<std::vector<std::string>>();
}

} // namespace fastecu::ui
