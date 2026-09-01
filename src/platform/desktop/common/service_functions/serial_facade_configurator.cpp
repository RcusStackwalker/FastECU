#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

#include <QString>

#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::service_functions
{

Status SerialPortActionsConfigurator::apply(const SsmTransportConfig& config)
{
    if (serial_ == nullptr)
    {
        return fail(ErrorKind::Disconnected, "no serial facade");
    }

    serial_->reset_connection();
    if (config.framing == SsmTransportConfig::Framing::Iso15765)
    {
        // legacy :70 -- FlashUtils::configureIso15765Can(serial, "500000", 0x7E1, 0x7E9).
        serial_->set_is_iso14230_connection(false);
        serial_->set_is_can_connection(false);
        serial_->set_is_iso15765_connection(true);
        serial_->set_is_29_bit_id(false);
        serial_->set_can_speed(QString::number(config.bitrate_or_baud));
        serial_->set_iso15765_source_address(config.request_id);
        serial_->set_iso15765_destination_address(config.response_id);
        serial_->set_can_source_address(config.request_id);
        serial_->set_can_destination_address(config.response_id);
        serial_->open_serial_port();
    }
    else
    {
        // legacy :141-152 -- "CAN 0xb8 command is disabled, so switch to
        // K-Line comms". Note the legacy order: open first, then set the
        // speed, then turn the driver's auto-header off.
        serial_->set_is_can_connection(false);
        serial_->set_is_iso15765_connection(false);
        serial_->set_is_iso14230_connection(true);
        serial_->open_serial_port();
        serial_->change_port_speed(QString::number(config.bitrate_or_baud));
        serial_->set_add_iso14230_header(config.add_iso14230_header);
    }

    if (!serial_->is_serial_port_open())
    {
        return fail(ErrorKind::Disconnected, "serial port did not open");
    }
    return {};
}

} // namespace fastecu::service_functions
