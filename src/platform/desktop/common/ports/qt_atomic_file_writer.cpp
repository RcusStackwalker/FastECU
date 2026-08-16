#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include <QSaveFile>

fastecu::Status QtAtomicFileWriter::replace(std::string_view handle, std::span<const std::uint8_t> data)
{
    QSaveFile file(QString::fromUtf8(handle.data(), static_cast<qsizetype>(handle.size())));
    if (!file.open(QIODevice::WriteOnly))
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, file.errorString().toStdString());
    }
    if (file.write(reinterpret_cast<const char *>(data.data()), static_cast<qint64>(data.size())) !=
        static_cast<qint64>(data.size()))
    {
        file.cancelWriting();
        return fastecu::fail(fastecu::ErrorKind::Internal, file.errorString().toStdString());
    }
    if (!file.commit())
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, file.errorString().toStdString());
    }
    return {};
}
