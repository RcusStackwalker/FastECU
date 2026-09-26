#pragma once

#include <functional>
#include <memory>
#include <variant>

#include <QString>

class QObject;
class SerialBackend;
class SerialPortActions;

// Constructor-only entry point to the serial facade for the desktop
// composition root; see this target's BUILD.bazel comment for why it exists.
// SerialPortActions stays an incomplete type for callers.
struct SerialPortActionsDeleter
{
    void operator()(SerialPortActions *serial) const;
};

using OwnedSerialPortActions = std::unique_ptr<SerialPortActions, SerialPortActionsDeleter>;

// Which backend the facade drives: the local adapter, or a FastECU remote
// peer. The composition root decides; the facade never knows.
struct DirectSerial
{
};

struct RemoteSerial
{
    QString address;
    QString password;
};

using SerialConnection = std::variant<DirectSerial, RemoteSerial>;

// The backend factory for a connection. The facade calls it once, lazily, on
// its I/O thread; separate from make_serial_port_actions so the choice can be
// tested without starting that thread.
std::function<SerialBackend *()> make_serial_backend_factory(const SerialConnection& connection);

// Builds the facade over the connection's backend and routes its
// LOG_E/LOG_W/LOG_I/LOG_D signals to log_sink's
// log_messages(QString, bool, bool) slot.
OwnedSerialPortActions make_serial_port_actions(const SerialConnection& connection, QObject& log_sink);
