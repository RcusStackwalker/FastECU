#ifndef DTC_PARSER_H
#define DTC_PARSER_H

#include <QHash>
#include <QString>

#include <cstdint>

class DtcParser
{
  public:
    static QString parse(uint16_t dtc,
                         const QHash<int, QString>& pCodes,
                         const QHash<int, QString>& cCodes,
                         const QHash<int, QString>& bCodes,
                         const QHash<int, QString>& uCodes);
};

#endif // DTC_PARSER_H
