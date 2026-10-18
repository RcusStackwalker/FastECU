#include "src/platform/desktop/common/serial/desktop_serial_factory.h"

#include <array>

#include "src/platform/desktop/common/serial/direct_serial_backend.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

void SerialPortActionsDeleter::operator()(SerialPortActions *serial) const
{
    delete serial;
}

std::function<SerialBackend *()> MakeSerialBackendFactory()
{
    return [] { return MakeDirectSerialBackend().release(); };
}

OwnedSerialPortActions MakeSerialPortActions(QObject& log_sink)
{
    auto serial = std::make_unique<SerialPortActions>(MakeSerialBackendFactory());
    // String-based connections, so log_sink can be any QObject with a
    // log_messages(QString, bool, bool) slot (SystemLogger in production).
    // SystemLogger::log_messages reads sender()'s signal to pick the level,
    // which a direct signal-to-slot connection preserves.
    const auto log_signals = std::to_array<const char *>({
        SIGNAL(logE(QString, bool, bool)),
        SIGNAL(logW(QString, bool, bool)),
        SIGNAL(logI(QString, bool, bool)),
        SIGNAL(logD(QString, bool, bool)),
    });
    for (const char *signal : log_signals)
    {
        QObject::connect(serial.get(), signal, &log_sink, SLOT(logMessages(QString, bool, bool)));
    }
    return OwnedSerialPortActions{serial.release()};
}
