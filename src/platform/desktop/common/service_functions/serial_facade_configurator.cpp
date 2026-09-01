#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

#include <QString>

#include <exception>

#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::service_functions
{

Status SerialPortActionsConfigurator::apply(const SsmTransportConfig& config)
{
    if (serial_ == nullptr)
    {
        return fail(ErrorKind::Disconnected, "no serial facade");
    }

    try
    {
        if (!serial_->reset_connection())
        {
            return fail(ErrorKind::Internal, "reset_connection failed");
        }
        if (config.framing == SsmTransportConfig::Framing::Iso15765)
        {
            // legacy :70 -- FlashUtils::configureIso15765Can(serial,
            // "500000", 0x7E1, 0x7E9). Check every setter result and clear
            // the K-Line auto-header as part of the mode transition so stale
            // state cannot alter these sessions' self-framed requests.
            if (!serial_->set_is_iso14230_connection(false))
            {
                return fail(ErrorKind::InvalidConfig, "set_is_iso14230_connection failed");
            }
            if (!serial_->set_is_can_connection(false))
            {
                return fail(ErrorKind::InvalidConfig, "set_is_can_connection failed");
            }
            if (!serial_->set_is_iso15765_connection(true))
            {
                return fail(ErrorKind::InvalidConfig, "set_is_iso15765_connection failed");
            }
            if (!serial_->set_is_29_bit_id(false))
            {
                return fail(ErrorKind::InvalidConfig, "set_is_29_bit_id failed");
            }
            if (!serial_->set_add_iso14230_header(config.add_iso14230_header))
            {
                return fail(ErrorKind::InvalidConfig, "set_add_iso14230_header failed");
            }
            if (!serial_->set_can_speed(QString::number(config.bitrate_or_baud)))
            {
                return fail(ErrorKind::InvalidConfig, "set_can_speed failed");
            }
            if (!serial_->set_iso15765_source_address(config.request_id))
            {
                return fail(ErrorKind::InvalidConfig, "set_iso15765_source_address failed");
            }
            if (!serial_->set_iso15765_destination_address(config.response_id))
            {
                return fail(ErrorKind::InvalidConfig, "set_iso15765_destination_address failed");
            }
            if (!serial_->set_can_source_address(config.request_id))
            {
                return fail(ErrorKind::InvalidConfig, "set_can_source_address failed");
            }
            if (!serial_->set_can_destination_address(config.response_id))
            {
                return fail(ErrorKind::InvalidConfig, "set_can_destination_address failed");
            }

            if (serial_->open_serial_port().isEmpty())
            {
                return fail(ErrorKind::Disconnected, "serial port did not open");
            }
            if (!serial_->is_serial_port_open())
            {
                return fail(ErrorKind::Disconnected, "serial port is not open after ISO-15765 setup");
            }
            return {};
        }

        // legacy :141-152 -- "CAN 0xb8 command is disabled, so switch to
        // K-Line comms": mode, open, baud, then auto-header. Check the port
        // both after open and after the baud call so a drop never falls
        // through into live service I/O.
        if (!serial_->set_is_can_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_can_connection failed");
        }
        if (!serial_->set_is_iso15765_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso15765_connection failed");
        }
        if (!serial_->set_is_iso14230_connection(true))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso14230_connection failed");
        }
        if (serial_->open_serial_port().isEmpty())
        {
            return fail(ErrorKind::Disconnected, "serial port did not open");
        }
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "serial port is not open before K-Line baud change");
        }

        const int baud_result = serial_->change_port_speed(QString::number(config.bitrate_or_baud));
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "serial port closed during K-Line baud change");
        }
        if (baud_result != 0)
        {
            return fail(ErrorKind::Internal, "K-Line driver rejected baud change");
        }
        if (!serial_->set_add_iso14230_header(config.add_iso14230_header))
        {
            return fail(ErrorKind::InvalidConfig, "set_add_iso14230_header failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "serial facade configuration exception");
    }
}

} // namespace fastecu::service_functions
