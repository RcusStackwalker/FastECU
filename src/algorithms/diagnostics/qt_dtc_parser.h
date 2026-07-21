#ifndef QT_DTC_PARSER_H
#define QT_DTC_PARSER_H

#include "src/algorithms/diagnostics/dtc_parser.h"

#include <QHash>
#include <QString>

#include <cstdint>

// TRANSITIONAL Qt shim: preserves the pre-conversion DtcParser::parse
// QHash<int,QString> API for callers not yet converted (backend FileActions
// still holds its P/C/B/U DTC lookup tables as QHash statics, defined in
// error_codes.h), delegating to the portable dtc_description() free
// function in dtc_parser.h.
class DtcParser
{
  public:
    static QString parse(uint16_t dtc,
                         const QHash<int, QString>& pCodes,
                         const QHash<int, QString>& cCodes,
                         const QHash<int, QString>& bCodes,
                         const QHash<int, QString>& uCodes);
};

#endif // QT_DTC_PARSER_H
