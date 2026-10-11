#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

bool IsJ2534CapableEntry(QStringView entry)
{
    return !entry.isEmpty();
}
