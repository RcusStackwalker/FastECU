#include "src/platform/desktop/common/ports/qt_clock.h"
#include <QElapsedTimer>
#include <QThread>

std::uint64_t QtClock::now_ms() const
{
    static QElapsedTimer base = []
    {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return static_cast<std::uint64_t>(base.elapsed());
}

fastecu::Status QtClock::sleep(int ms, const fastecu::ICancellationToken& t)
{
    const int slice = 10;
    int remaining = ms;
    while (remaining > 0)
    {
        if (t.cancelled())
            return fastecu::fail(fastecu::ErrorKind::Cancelled);
        int step = remaining < slice ? remaining : slice;
        QThread::msleep(static_cast<unsigned long>(step));
        remaining -= step;
    }
    return {};
}
