#ifndef FLASH_UTILS_H
#define FLASH_UTILS_H

#include <QByteArray>
#include <QString>

#include <kernelmemorymodels.h>

class SerialPortActions;

namespace FlashUtils
{
int findFlashDeviceIndex(const QString& mcuType);
const flashdev_t *findFlashDevice(const QString& mcuType);

QByteArray buildIso15765Request(quint32 sourceAddress, const QByteArray& payload);
void configureIso15765Can(SerialPortActions *serial,
                          const QString& canSpeed,
                          quint32 sourceAddress,
                          quint32 destinationAddress,
                          bool use29BitId = false);
} // namespace FlashUtils

#endif // FLASH_UTILS_H
