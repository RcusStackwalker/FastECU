#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/result.h"

class SerialBackend;

namespace fastecu::flash
{

// Device selection plus construction for the desktop CAN flash transport.
//
// Exists so a consumer outside src/platform can obtain an ICanFlashTransport
// without naming SerialPortActions. A direct dependency on serial_qt_compat
// would require adding that consumer to its visibility list, which
// //:serial_compat_allowlist freezes as "may shrink, never grow".
//
// Performs the sequence MainWindow does by hand (mainwindow.cpp:347, 448, 479):
// construct SerialPortActions, check_serial_ports(), set_serial_port_list(),
// wrap in DesktopCanFlashTransport's owning constructor, configure(), open().
struct DesktopCanTransportConfig
{
    // Empty selects the first detected device.
    std::string port_name;

    // Builds the serial backend the facade drives; required. Production
    // passes make_serial_backend_factory(DirectSerial{}) from
    // desktop_serial_factory.h; tests pass a fake.
    std::function<SerialBackend *()> backend_factory;
};

Result<std::vector<std::string>> list_desktop_serial_ports(const DesktopCanTransportConfig& config);

Result<std::unique_ptr<ICanFlashTransport>> open_desktop_can_flash_transport(const DesktopCanTransportConfig& config,
                                                                             const Iso15765Config& can);

} // namespace fastecu::flash
