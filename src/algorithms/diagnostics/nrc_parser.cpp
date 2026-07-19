#include "src/algorithms/diagnostics/nrc_parser.h"

QString NrcParser::parse(const QByteArray& nrc, const QHash<int, QString>& codes)
{
    if (nrc.length() < 3 || static_cast<unsigned char>(nrc.at(0)) != 0x7f)
    {
        return "Not a valid answer";
    }

    return codes.value(static_cast<unsigned char>(nrc.at(2)), "Unknown error code");
}
