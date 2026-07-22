#include "src/algorithms/diagnostics/qt_dtc_parser.h"

#include <array>
#include <string>
#include <unordered_map>

namespace
{

std::unordered_map<int, std::string> toPortable(const QHash<int, QString>& codes)
{
    std::unordered_map<int, std::string> result;
    result.reserve(static_cast<std::size_t>(codes.size()));
    for (auto it = codes.constBegin(); it != codes.constEnd(); ++it)
    {
        result.emplace(it.key(), it.value().toStdString());
    }
    return result;
}

} // namespace

QString DtcParser::parse(uint16_t dtc,
                         const QHash<int, QString>& pCodes,
                         const QHash<int, QString>& cCodes,
                         const QHash<int, QString>& bCodes,
                         const QHash<int, QString>& uCodes)
{
    // dtc_description() only ever consults the table matching dtc's
    // category; converting the other three (each holding up to several
    // thousand DTC entries) on every call would be wasted work, so convert
    // only the one QHash this lookup needs.
    static const std::unordered_map<int, std::string> kEmpty;
    const std::array<const QHash<int, QString> *, 4> tables = {&pCodes, &cCodes, &bCodes, &uCodes};
    const unsigned category = dtc >> 14;

    const std::unordered_map<int, std::string> converted =
        category < tables.size() ? toPortable(*tables[category]) : kEmpty;

    const std::unordered_map<int, std::string>& p = category == 0 ? converted : kEmpty;
    const std::unordered_map<int, std::string>& c = category == 1 ? converted : kEmpty;
    const std::unordered_map<int, std::string>& b = category == 2 ? converted : kEmpty;
    const std::unordered_map<int, std::string>& u = category == 3 ? converted : kEmpty;

    return QString::fromStdString(dtc_description(dtc, p, c, b, u));
}
