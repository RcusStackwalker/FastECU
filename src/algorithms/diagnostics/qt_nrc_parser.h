#ifndef QT_NRC_PARSER_H
#define QT_NRC_PARSER_H

#include "src/algorithms/diagnostics/nrc_parser.h"

#include <QByteArray>
#include <QHash>
#include <QString>

// TRANSITIONAL Qt shim: preserves the pre-conversion NrcParser::parse
// QByteArray/QHash<int,QString> API for callers not yet converted (backend
// FileActions still holds its NRC lookup table as a QHash static, defined in
// error_codes.h), delegating to the portable nrc_description() free
// function in nrc_parser.h.
class NrcParser
{
  public:
    static QString parse(const QByteArray& nrc, const QHash<int, QString>& codes);
};

#endif // QT_NRC_PARSER_H
