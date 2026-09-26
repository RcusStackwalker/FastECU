#pragma once

#include <memory>

#include <QString>

class QObject;
class SerialPortActions;

// Constructor-only entry point to the serial facade for the desktop
// composition root; see this target's BUILD.bazel comment for why it exists.
// SerialPortActions stays an incomplete type for callers.
struct SerialPortActionsDeleter
{
    void operator()(SerialPortActions *serial) const;
};

using OwnedSerialPortActions = std::unique_ptr<SerialPortActions, SerialPortActionsDeleter>;

// Builds the facade (direct when peer_address is empty, remote otherwise) and
// routes its LOG_E/LOG_W/LOG_I/LOG_D signals to log_sink's
// log_messages(QString, bool, bool) slot.
OwnedSerialPortActions make_serial_port_actions(const QString& peer_address, const QString& peer_password,
                                                QObject& log_sink);
