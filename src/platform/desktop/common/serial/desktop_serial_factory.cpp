#include "src/platform/desktop/common/serial/desktop_serial_factory.h"

#include <array>

#include "src/platform/desktop/common/serial/serial_port_actions.h"

void SerialPortActionsDeleter::operator()(SerialPortActions *serial) const
{
    delete serial;
}

OwnedSerialPortActions make_serial_port_actions(const QString& peer_address, const QString& peer_password,
                                                QObject& log_sink)
{
    auto serial = std::make_unique<SerialPortActions>(peer_address, peer_password, nullptr, nullptr);
    // String-based connections, so log_sink can be any QObject with a
    // log_messages(QString, bool, bool) slot (SystemLogger in production).
    // SystemLogger::log_messages reads sender()'s signal to pick the level,
    // which a direct signal-to-slot connection preserves.
    const auto log_signals = std::to_array<const char *>({
        SIGNAL(LOG_E(QString, bool, bool)),
        SIGNAL(LOG_W(QString, bool, bool)),
        SIGNAL(LOG_I(QString, bool, bool)),
        SIGNAL(LOG_D(QString, bool, bool)),
    });
    for (const char *signal : log_signals)
    {
        QObject::connect(serial.get(), signal, &log_sink, SLOT(log_messages(QString, bool, bool)));
    }
    return OwnedSerialPortActions{serial.release()};
}
