#include "src/platform/desktop/common/transport/desktop_transport_factory.h"

#include <QString>
#include <QStringList>

#include <algorithm>
#include <format>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"
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
        return fail(ErrorKind::Disconnected, "no serial ports detected");
    }

    QString wanted;
    if (config.port_name.empty())
    {
        // Not detected.front(): check_serial_ports() returns every serial
        // port sorted alphabetically, so on macOS the first entry is
        // cu.Bluetooth-Incoming-Port. open_serial_port() would then degrade
        // it to a plain serial port and report success, and every ISO-15765
        // exchange would time out against a port with no ECU behind it.
        const auto adapter = std::ranges::find_if(detected, isJ2534CapableEntry);
        if (adapter == detected.end())
        {
            // Distinct from the empty-list case above: ports were found, none
            // of them is an adapter. Different user action -- plug the adapter
            // in, rather than check the cable.
            return fail(ErrorKind::Disconnected,
                        std::format("no J2534 adapter among {} detected serial ports", detected.size()));
        }
        wanted = *adapter;
    }
    else
    {
        // An absent *named* device is a configuration mistake, not a missing
        // adapter -- the adapter set was read successfully, the request just
        // did not match it. Kept distinct from the empty-list case above so an
        // agent can tell "plug something in" from "you typed the wrong name".
        wanted = QString::fromStdString(config.port_name);
        if (!detected.contains(wanted))
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("no such device: {} (detected {})", wanted.toStdString(), detected.size()));
        }
        // Accepting a named non-J2534 port only buys one read timeout per
        // exchange: ISO-15765 cannot run over a plain serial port.
        if (!isJ2534CapableEntry(wanted))
        {
            return fail(ErrorKind::InvalidConfig, std::format("not a J2534 adapter: {}", wanted.toStdString()));
        }
    }
    // open_serial_port() consumes the selected UI-style entry from
    // serial_port_list.at(0), including the adapter description used to select
    // the J2534 path. set_serial_port() only updates a separate scalar and
    // leaves that list empty, which makes the direct backend assert on open.
    if (!serial->set_serial_port_list(QStringList{wanted}))
    {
        return fail(ErrorKind::InvalidConfig, std::format("set_serial_port_list({}) failed", wanted.toStdString()));
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
