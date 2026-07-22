#include "src/algorithms/diagnostics/qt_nrc_parser.h"

#include "src/algorithms/protocol/qt_bytes.h"

#include <string>
#include <unordered_map>

QString NrcParser::parse(const QByteArray& nrc, const QHash<int, QString>& codes)
{
    std::unordered_map<int, std::string> portableCodes;
    portableCodes.reserve(static_cast<std::size_t>(codes.size()));
    for (auto it = codes.constBegin(); it != codes.constEnd(); ++it)
    {
        portableCodes.emplace(it.key(), it.value().toStdString());
    }

    return QString::fromStdString(nrc_description(bytes::view(nrc), portableCodes));
}
