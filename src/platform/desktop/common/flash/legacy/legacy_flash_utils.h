#pragma once

#include <QString>

#include "src/backend/flash/flash_device_lookup.h"

class SerialPortActions;

// TRANSITIONAL. Relocated from //src/backend/flash in step 5e so that no
// backend production target depends on src/platform. The FlashUtils
// namespace name is retained so the 28 legacy call sites changed only their
// #include.
//
// findFlashDeviceIndex is a QString-typed shim over the portable
// fastecu::flash::find_flash_device_index; each flash family that migrates
// in the step-5 tail should convert its own call sites to the portable form,
// and this shim is deleted when the last one does.
namespace FlashUtils
{
int findFlashDeviceIndex(const QString& mcuType);

void configureIso15765Can(SerialPortActions *serial,
                          const QString& canSpeed,
                          quint32 sourceAddress,
                          quint32 destinationAddress,
                          bool use29BitId = false);
} // namespace FlashUtils
