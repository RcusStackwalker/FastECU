#include "src/platform/desktop/common/flash/legacy/legacy_flash_utils.h"

#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace FlashUtils
{

int findFlashDeviceIndex(const QString& mcuType)
{
    return fastecu::flash::find_flash_device_index(mcuType.toStdString());
}

void configureIso15765Can(SerialPortActions *serial, const QString& canSpeed, quint32 sourceAddress,
                          quint32 destinationAddress, bool use29BitId)
{
    serial->set_is_iso14230_connection(false);
    serial->set_add_iso14230_header(false);
    serial->set_is_can_connection(false);
    serial->set_is_iso15765_connection(true);
    serial->set_is_29_bit_id(use29BitId);
    serial->set_can_speed(canSpeed);
    serial->set_iso15765_source_address(sourceAddress);
    serial->set_iso15765_destination_address(destinationAddress);
}

} // namespace FlashUtils
