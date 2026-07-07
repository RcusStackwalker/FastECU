#include "dtc_parser.h"

namespace {

QString defaultDtcMessage(uint16_t dtc)
{
    static const char prefixes[] = { 'P', 'C', 'B', 'U' };
    const int category = dtc >> 14;
    const uint16_t code = dtc & 0x3fff;

    return QString("%1%2 - Unknown error code")
        .arg(prefixes[category])
        .arg(code, 4, 16, QLatin1Char('0'));
}

}

QString DtcParser::parse(uint16_t dtc,
                         const QHash<int, QString> &pCodes,
                         const QHash<int, QString> &cCodes,
                         const QHash<int, QString> &bCodes,
                         const QHash<int, QString> &uCodes)
{
    const QString fallback = defaultDtcMessage(dtc);

    switch (dtc >> 14) {
    case 0x00:
        return pCodes.value(dtc, fallback);
    case 0x01:
        return cCodes.value(dtc, fallback);
    case 0x02:
        return bCodes.value(dtc, fallback);
    case 0x03:
        return uCodes.value(dtc, fallback);
    default:
        return fallback;
    }
}
