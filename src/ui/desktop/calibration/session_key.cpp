#include "src/ui/desktop/calibration/session_key.h"

#include <algorithm>
#include <cstdint>

namespace fastecu::ui
{

QString session_key_text(calibration::SessionId id)
{
    return QString::number(static_cast<qulonglong>(static_cast<std::uint64_t>(id)));
}

std::optional<calibration::SessionId> parse_session_key(const QString& text)
{
    if (text.isEmpty() || !std::ranges::all_of(text, [](QChar c) { return c >= u'0' && c <= u'9'; }))
    {
        return std::nullopt;
    }
    bool ok = false;
    const qulonglong value = text.toULongLong(&ok);
    if (!ok)
    {
        return std::nullopt;
    }
    return calibration::SessionId{static_cast<std::uint64_t>(value)};
}

} // namespace fastecu::ui
