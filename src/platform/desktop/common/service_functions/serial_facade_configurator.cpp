#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

#include <QString>

#include <exception>
#include <format>
#include <functional>
#include <initializer_list>

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

namespace fastecu::service_functions
{
namespace
{

struct Setter
{
    const char *name;
    std::function<bool()> apply;
};

// Runs each setter in order and stops at the first one the facade rejects.
Status ApplySetters(std::initializer_list<Setter> setters)
{
    for (const Setter& setter : setters)
    {
        if (!setter.apply())
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("{} failed", setter.name));
        }
    }
    return {};
}

Status OpenPort(SerialPortActions& serial, const char *not_open)
{
    if (serial.OpenSerialPort().isEmpty())
    {
        return Fail(ErrorKind::kDisconnected, "serial port did not open");
    }
    if (!serial.IsSerialPortOpen())
    {
        return Fail(ErrorKind::kDisconnected, not_open);
    }
    return {};
}

// FlashUtils::configureIso15765Can(serial, "500000", 0x7E1, 0x7E9). Check every
// setter result and clear the K-Line auto-header as part of the mode transition
// so stale state cannot alter these sessions' self-framed requests.
Status ConfigureIso15765(SerialPortActions& serial, const SsmTransportConfig& config)
{
    return ApplySetters(
               {
                   {"set_is_iso14230_connection", [&] { return serial.SetIsIso14230Connection(false); }},
                   {"set_is_can_connection", [&] { return serial.SetIsCanConnection(false); }},
                   {"set_is_iso15765_connection", [&] { return serial.SetIsIso15765Connection(true); }},
                   {"set_is_29_bit_id", [&] { return serial.SetIs29BitId(false); }},
                   {"set_add_iso14230_header", [&] { return serial.SetAddIso14230Header(config.add_iso14230_header); }},
                   {"set_can_speed", [&] { return serial.SetCanSpeed(QString::number(config.bitrate_or_baud)); }},
                   {"set_iso15765_source_address", [&] { return serial.SetIso15765SourceAddress(config.request_id); }},
                   {"set_iso15765_destination_address",
                    [&] { return serial.SetIso15765DestinationAddress(config.response_id); }},
                   {"set_can_source_address", [&] { return serial.SetCanSourceAddress(config.request_id); }},
                   {"set_can_destination_address", [&] { return serial.SetCanDestinationAddress(config.response_id); }},
               })
        .and_then([&] { return OpenPort(serial, "serial port is not open after ISO-15765 setup"); });
}

// "CAN 0xb8 command is disabled, so switch to K-Line comms": mode, open, baud,
// then auto-header. Check the port both after open and after the baud call so a
// drop never falls through into live service I/O.
Status ConfigureKLine(SerialPortActions& serial, const SsmTransportConfig& config)
{
    return ApplySetters({
                            {"set_is_can_connection", [&] { return serial.SetIsCanConnection(false); }},
                            {"set_is_iso15765_connection", [&] { return serial.SetIsIso15765Connection(false); }},
                            {"set_is_iso14230_connection", [&] { return serial.SetIsIso14230Connection(true); }},
                        })
        .and_then([&] { return OpenPort(serial, "serial port is not open before K-Line baud change"); })
        .and_then(
            [&]() -> Status
            {
                const int baud_result = serial.ChangePortSpeed(QString::number(config.bitrate_or_baud));
                if (!serial.IsSerialPortOpen())
                {
                    return Fail(ErrorKind::kDisconnected, "serial port closed during K-Line baud change");
                }
                if (baud_result != 0)
                {
                    return Fail(ErrorKind::kInternal, "K-Line driver rejected baud change");
                }
                return ApplySetters({{"set_add_iso14230_header",
                                      [&] { return serial.SetAddIso14230Header(config.add_iso14230_header); }}});
            });
}

} // namespace

Status SerialPortActionsConfigurator::Apply(const SsmTransportConfig& config)
{
    if (serial_ == nullptr)
    {
        return Fail(ErrorKind::kDisconnected, "no serial facade");
    }

    try
    {
        if (!serial_->ResetConnection())
        {
            return Fail(ErrorKind::kInternal, "reset_connection failed");
        }
        return config.framing == SsmTransportConfig::Framing::kIso15765 ? ConfigureIso15765(*serial_, config)
                                                                        : ConfigureKLine(*serial_, config);
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "serial facade configuration exception");
    }
}

} // namespace fastecu::service_functions
