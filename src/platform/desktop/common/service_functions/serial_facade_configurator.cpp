#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

#include <QString>

#include <exception>

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

namespace fastecu::service_functions
{

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
        if (config.framing == SsmTransportConfig::Framing::kIso15765)
        {
            // legacy :70 -- FlashUtils::configureIso15765Can(serial,
            // "500000", 0x7E1, 0x7E9). Check every setter result and clear
            // the K-Line auto-header as part of the mode transition so stale
            // state cannot alter these sessions' self-framed requests.
            if (!serial_->SetIsIso14230Connection(false))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_is_iso14230_connection failed");
            }
            if (!serial_->SetIsCanConnection(false))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_is_can_connection failed");
            }
            if (!serial_->SetIsIso15765Connection(true))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_is_iso15765_connection failed");
            }
            if (!serial_->SetIs29BitId(false))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_is_29_bit_id failed");
            }
            if (!serial_->SetAddIso14230Header(config.add_iso14230_header))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_add_iso14230_header failed");
            }
            if (!serial_->SetCanSpeed(QString::number(config.bitrate_or_baud)))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_can_speed failed");
            }
            if (!serial_->SetIso15765SourceAddress(config.request_id))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_iso15765_source_address failed");
            }
            if (!serial_->SetIso15765DestinationAddress(config.response_id))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_iso15765_destination_address failed");
            }
            if (!serial_->SetCanSourceAddress(config.request_id))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_can_source_address failed");
            }
            if (!serial_->SetCanDestinationAddress(config.response_id))
            {
                return Fail(ErrorKind::kInvalidConfig, "set_can_destination_address failed");
            }

            if (serial_->OpenSerialPort().isEmpty())
            {
                return Fail(ErrorKind::kDisconnected, "serial port did not open");
            }
            if (!serial_->IsSerialPortOpen())
            {
                return Fail(ErrorKind::kDisconnected, "serial port is not open after ISO-15765 setup");
            }
            return {};
        }

        // legacy :141-152 -- "CAN 0xb8 command is disabled, so switch to
        // K-Line comms": mode, open, baud, then auto-header. Check the port
        // both after open and after the baud call so a drop never falls
        // through into live service I/O.
        if (!serial_->SetIsCanConnection(false))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_can_connection failed");
        }
        if (!serial_->SetIsIso15765Connection(false))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_iso15765_connection failed");
        }
        if (!serial_->SetIsIso14230Connection(true))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_iso14230_connection failed");
        }
        if (serial_->OpenSerialPort().isEmpty())
        {
            return Fail(ErrorKind::kDisconnected, "serial port did not open");
        }
        if (!serial_->IsSerialPortOpen())
        {
            return Fail(ErrorKind::kDisconnected, "serial port is not open before K-Line baud change");
        }

        const int baud_result = serial_->ChangePortSpeed(QString::number(config.bitrate_or_baud));
        if (!serial_->IsSerialPortOpen())
        {
            return Fail(ErrorKind::kDisconnected, "serial port closed during K-Line baud change");
        }
        if (baud_result != 0)
        {
            return Fail(ErrorKind::kInternal, "K-Line driver rejected baud change");
        }
        if (!serial_->SetAddIso14230Header(config.add_iso14230_header))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_add_iso14230_header failed");
        }
        return {};
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
