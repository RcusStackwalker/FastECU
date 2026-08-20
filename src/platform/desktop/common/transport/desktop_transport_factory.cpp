#include "src/platform/desktop/common/transport/desktop_transport_factory.h"

#include <QString>
#include <QStringList>

#include <format>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"

namespace fastecu::flash
{
namespace
{

std::unique_ptr<SerialPortActions> make_serial(const DesktopCanTransportConfig& config)
{
    return std::make_unique<SerialPortActions>(QString::fromStdString(config.peer_address),
                                               QString::fromStdString(config.peer_password), nullptr, nullptr,
                                               config.backend_factory_for_tests);
}

} // namespace

Result<std::vector<std::string>> list_desktop_serial_ports(const DesktopCanTransportConfig& config)
{
    auto serial = make_serial(config);
    std::vector<std::string> ports;
    for (const QString& port : serial->check_serial_ports())
    {
        ports.push_back(port.toStdString());
    }
    return ports;
}

Result<std::unique_ptr<ICanFlashTransport>> open_desktop_can_flash_transport(const DesktopCanTransportConfig& config,
                                                                             const Iso15765Config& can)
{
    auto serial = make_serial(config);

    const QStringList detected = serial->check_serial_ports();
    if (detected.isEmpty())
    {
        return fail(ErrorKind::Disconnected, "no J2534 device detected");
    }

    // An absent *named* device is a configuration mistake, not a missing
    // adapter -- the adapter set was read successfully, the request just did
    // not match it. Kept distinct from the empty-list case above so an agent
    // can tell "plug something in" from "you typed the wrong name".
    const QString wanted = config.port_name.empty() ? detected.front() : QString::fromStdString(config.port_name);
    if (!detected.contains(wanted))
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("no such device: {} (detected {})", wanted.toStdString(), detected.size()));
    }
    if (!serial->set_serial_port(wanted))
    {
        return fail(ErrorKind::InvalidConfig, std::format("set_serial_port({}) failed", wanted.toStdString()));
    }

    auto transport = std::make_unique<DesktopCanFlashTransport>(std::move(serial));
    if (const Status configured = transport->configure(can); !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    if (const Status opened = transport->open(); !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    return transport;
}

} // namespace fastecu::flash
