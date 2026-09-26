#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

bool isJ2534CapableEntry(QStringView entry)
{
    return !entry.isEmpty();
}
