#include "src/platform/desktop/common/flash/flash_event_adapter.h"

namespace fastecu::flash
{

void QtEventSinkAdapter::log(LogLevel level, std::string_view message)
{
    emit logEvent(static_cast<int>(level),
                  QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
}

void QtEventSinkAdapter::progress(int done, int total)
{
    emit progressChanged(done, total);
}

void QtEventSinkAdapter::notice(std::string_view message)
{
    emit logEvent(static_cast<int>(LogLevel::Info),
                  QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
}

} // namespace fastecu::flash
