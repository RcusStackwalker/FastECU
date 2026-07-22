#include "src/platform/desktop/common/ports/qt_event_sink.h"

void QtEventSink::log(fastecu::LogLevel lvl, std::string_view message)
{
    emit logged(static_cast<int>(lvl),
                QString::fromUtf8(message.data(), static_cast<int>(message.size())));
}
void QtEventSink::progress(int done, int total)
{
    emit progressed(done, total);
}
void QtEventSink::notice(std::string_view message)
{
    emit noticed(QString::fromUtf8(message.data(), static_cast<int>(message.size())));
}
