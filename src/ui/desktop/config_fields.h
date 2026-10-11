#pragma once
#include <string>
#include <string_view>
#include <vector>

#include <QString>
#include <QStringList>

#include "src/backend/config/catalog.h"

namespace fastecu::ui
{

inline QString qs(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// A text field of the vehicle's protocol, for display.
QString protocolField(const config::VehicleSpec& vehicle, std::string_view config::ProtocolSpec::*field);

// "yes" or "no" for a capability, as the vehicle chooser has always shown it.
QString protocolFlag(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability);

// Whether the vehicle's protocol offers a capability.
bool protocolCapability(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability);

// "yes", "n/a" or "no".
QString checksumField(const config::VehicleSpec& vehicle);

// "0xFFFF3000", or empty when the protocol uploads no kernel.
QString kernelAddressField(const config::VehicleSpec& vehicle);

QStringList qstringList(const std::vector<std::string>& items);
std::vector<std::string> stringVector(const QStringList& items);

} // namespace fastecu::ui
