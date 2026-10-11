#pragma once

#include <optional>

#include <QString>

#include "src/backend/calibration/session/calibration_session.h"

namespace fastecu::ui
{

// How a calibration session is named in the widgets that refer to it: the
// files tree's column 2 and the first field of a map window's object name.
// Decimal, so the object-name shape "<key>,<map>,<name>" is unchanged from the
// positional index it replaces.
QString sessionKeyText(calibration::SessionId id);

// Strict inverse of session_key_text: plain decimal digits only.
std::optional<calibration::SessionId> parseSessionKey(const QString& text);

} // namespace fastecu::ui
