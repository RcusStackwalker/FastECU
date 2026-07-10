#ifndef FLASH_UTILS_H
#define FLASH_UTILS_H

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <span>

#include <kernelmemorymodels.h>

class SerialPortActions;

namespace FlashUtils
{
int findFlashDeviceIndex(const QString& mcuType);
const flashdev_t *findFlashDevice(const QString& mcuType);

// One's-complement-style 8-bit checksum: sum bytes, and whenever the
// running sum overflows 8 bits, add 1 back in before truncating (rather
// than a plain mod-256 sum). Used to checksum ECU reflash blocks.
std::uint8_t cks_add8(std::span<const std::uint8_t> data);

QByteArray buildIso15765Request(quint32 sourceAddress, const QByteArray& payload);
void configureIso15765Can(SerialPortActions *serial,
                          const QString& canSpeed,
                          quint32 sourceAddress,
                          quint32 destinationAddress,
                          bool use29BitId = false);
} // namespace FlashUtils

#endif // FLASH_UTILS_H
