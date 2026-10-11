#include "src/platform/desktop/common/ports/event_sink/qt_event_sink.h"

void QtEventSink::Log(fastecu::LogLevel lvl, std::string_view message)
{
    emit logged(static_cast<int>(lvl), QString::fromUtf8(message.data(), static_cast<int>(message.size())));
}
void QtEventSink::Progress(int done, int total)
{
    emit progressed(done, total);
}
void QtEventSink::PhaseProgress(const fastecu::PhaseProgressEvent& event)
{
    Progress(event.done, event.total);
    emit phaseProgressed(QString::fromUtf8(event.phase_name.data(), static_cast<int>(event.phase_name.size())),
                         event.phase_index, event.phase_count, event.done, event.total);
}
void QtEventSink::Notice(std::string_view message)
{
    emit noticed(QString::fromUtf8(message.data(), static_cast<int>(message.size())));
}
