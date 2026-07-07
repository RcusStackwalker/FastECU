#ifndef NRC_PARSER_H
#define NRC_PARSER_H

#include <QByteArray>
#include <QHash>
#include <QString>

class NrcParser
{
  public:
    static QString parse(const QByteArray& nrc, const QHash<int, QString>& codes);
};

#endif // NRC_PARSER_H
