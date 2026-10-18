#pragma once

#include <functional>
#include <memory>

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

// The direct backend factory. The facade calls it once, lazily, on its I/O
// thread; separate from MakeSerialPortActions so the backend can be tested
// without starting that thread.
std::function<SerialBackend *()> MakeSerialBackendFactory();

// Builds the facade over the direct backend and routes its
// logE/logW/logI/logD signals to log_sink's
// logMessages(QString, bool, bool) slot.
OwnedSerialPortActions MakeSerialPortActions(QObject& log_sink);
