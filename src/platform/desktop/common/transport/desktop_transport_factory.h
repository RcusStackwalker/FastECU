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
// without naming SerialPortActions, whose target is visible only to
// src/platform/desktop and //tests.
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

Result<std::vector<std::string>> ListDesktopSerialPorts(const DesktopCanTransportConfig& config);

Result<std::unique_ptr<ICanFlashTransport>> OpenDesktopCanFlashTransport(const DesktopCanTransportConfig& config,
                                                                         const Iso15765Config& can);

} // namespace fastecu::flash
