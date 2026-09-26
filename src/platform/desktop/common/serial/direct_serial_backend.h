#pragma once

#include <memory>

class SerialBackend;

// Builds the local (serial port / J2534 adapter) backend. The only way code
// outside the direct backend's own target obtains one, so it never names
// SerialPortActionsDirect or includes a J2534 header.
std::unique_ptr<SerialBackend> make_direct_serial_backend();
