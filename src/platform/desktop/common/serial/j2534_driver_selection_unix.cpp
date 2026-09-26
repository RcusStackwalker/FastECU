#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

bool isJ2534CapableEntry(QStringView entry)
{
    const qsizetype separator = entry.indexOf(QStringView(u" - "));
    return separator >= 0 && entry.sliced(separator + 3).contains(QStringView(u"OpenPort 2.0"), Qt::CaseInsensitive);
}
