#ifndef CHECKSUM_RESULT_H
#define CHECKSUM_RESULT_H

#include <QByteArray>
#include <QString>

struct ChecksumResult
{
    enum class Status
    {
        Unchanged,
        Corrected,
        Disabled,
        InvalidSize,
        UnsupportedRom,
        ParseError
    };

    Status status = Status::Unchanged;
    QByteArray romData;
    QString message;

    bool changed() const
    {
        return status == Status::Corrected;
    }
    bool ok() const
    {
        return status == Status::Unchanged || status == Status::Corrected || status == Status::Disabled;
    }
};

#endif // CHECKSUM_RESULT_H
