#ifndef QT_CHECKSUM_H
#define QT_CHECKSUM_H

#include "src/algorithms/checksum/checksum_result.h"
#include "src/algorithms/protocol/qt_bytes.h"

#include <QByteArray>
#include <QString>

struct QtChecksumResult
{
    ChecksumResult::Status status = ChecksumResult::Status::Unchanged;
    QByteArray romData;
    QString message;

    bool changed() const
    {
        return status == ChecksumResult::Status::Corrected;
    }
    // Not in the task brief's illustrative snippet, but required at both real
    // call sites: file_actions.cpp's applyChecksumResult() gates the
    // FullRomData write-back on it, and tests/test_checksum_results.cpp (the
    // frozen Qt contract this shim retargets) asserts it directly. Mirrors
    // ChecksumResult::ok() exactly.
    bool ok() const
    {
        return status == ChecksumResult::Status::Unchanged || status == ChecksumResult::Status::Corrected ||
               status == ChecksumResult::Status::Disabled;
    }
};

inline QtChecksumResult toQt(const ChecksumResult& result)
{
    QtChecksumResult out;
    out.status = result.status;
    out.romData = bytes::toQByteArray(bytes::ByteView(result.romData));
    out.message = QString::fromStdString(result.message);
    return out;
}

#endif // QT_CHECKSUM_H
