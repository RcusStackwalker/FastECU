#include "flash_utils.h"
#include "protocol/qt_bytes.h"
#include "serial_port_actions.h"

namespace FlashUtils
{
std::uint8_t cks_add8(std::span<const std::uint8_t> data)
{
    std::uint16_t sum = 0;
    for (std::uint8_t byte : data)
    {
        sum += byte;
        if (sum & 0x100)
        {
            sum += 1;
        }
        sum = static_cast<std::uint8_t>(sum);
    }
    return static_cast<std::uint8_t>(sum);
}

int findFlashDeviceIndex(const QString& mcuType)
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

const flashdev_t *findFlashDevice(const QString& mcuType)
{
    const int index = findFlashDeviceIndex(mcuType);
    if (index < 0)
        return nullptr;
    return &flashdevices[index];
}

QByteArray buildIso15765Request(quint32 sourceAddress, const QByteArray& payload)
{
    QByteArray output;
    bytes::appendU32Be(output, sourceAddress);
    output.append(payload);
    return output;
}

void configureIso15765Can(SerialPortActions *serial,
                          const QString& canSpeed,
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
} // namespace FlashUtils
