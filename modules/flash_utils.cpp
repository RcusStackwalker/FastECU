#include "flash_utils.h"
#include "serial_port_actions.h"

namespace FlashUtils
{
int findFlashDeviceIndex(const QString &mcuType)
{
    int index = 0;
    while (flashdevices[index].name != nullptr)
    {
        if (QString::fromLatin1(flashdevices[index].name) == mcuType)
            return index;
        ++index;
    }
    return -1;
}

const flashdev_t *findFlashDevice(const QString &mcuType)
{
    const int index = findFlashDeviceIndex(mcuType);
    if (index < 0)
        return nullptr;
    return &flashdevices[index];
}

QByteArray buildIso15765Request(quint32 sourceAddress, const QByteArray &payload)
{
    QByteArray output;
    output.append(char((sourceAddress >> 24) & 0xFF));
    output.append(char((sourceAddress >> 16) & 0xFF));
    output.append(char((sourceAddress >> 8) & 0xFF));
    output.append(char(sourceAddress & 0xFF));
    output.append(payload);
    return output;
}

void configureIso15765Can(SerialPortActions *serial,
                          const QString &canSpeed,
                          quint32 sourceAddress,
                          quint32 destinationAddress,
                          bool use29BitId)
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
}
