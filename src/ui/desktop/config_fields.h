#pragma once
#include <string>
#include <string_view>
#include <vector>

#include <QString>
#include <QStringList>

#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/protocol_catalog.h"

namespace fastecu::ui
{

inline QString qs(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// A protocol field for display; the legacy single-space placeholder when the
// vehicle's protocol reference did not resolve.
QString protocol_field(const config::ResolvedCarModel& vehicle, std::string config::ProtocolEntry::*field);

// True only for a resolved protocol whose capability field reads "yes".
bool protocol_capability(const config::ResolvedCarModel& vehicle, std::string config::ProtocolEntry::*capability);

QStringList qstring_list(const std::vector<std::string>& items);
std::vector<std::string> string_vector(const QStringList& items);

} // namespace fastecu::ui
