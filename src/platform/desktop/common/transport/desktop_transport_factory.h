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
// construct SerialPortActions, check_serial_ports(), set_serial_port(), wrap in
// DesktopCanFlashTransport's owning constructor, configure(), open().
struct DesktopCanTransportConfig
{
    // Empty selects the first detected device.
    std::string port_name;
    // Empty means a local J2534 adapter rather than a remote host.
    std::string peer_address;
    std::string peer_password;

    // Test seam only: forwarded to SerialPortActions' backendFactoryForTests
    // parameter. Empty in production, which selects the real backend.
    std::function<SerialBackend *()> backend_factory_for_tests;
};

Result<std::vector<std::string>> list_desktop_serial_ports(const DesktopCanTransportConfig& config);

Result<std::unique_ptr<ICanFlashTransport>> open_desktop_can_flash_transport(const DesktopCanTransportConfig& config,
                                                                             const Iso15765Config& can);

} // namespace fastecu::flash
