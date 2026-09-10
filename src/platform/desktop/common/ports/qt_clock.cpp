#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/backend/ports/duration_cast.h"
#include <QElapsedTimer>
#include <QThread>

using namespace std::chrono_literals;

std::chrono::steady_clock::time_point QtClock::now() const
{
    static QElapsedTimer base = []
    {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{base.elapsed()};
}

fastecu::Status QtClock::sleep(std::chrono::milliseconds duration, const fastecu::ICancellationToken& t)
{
    constexpr auto slice = 10ms;
    auto remaining = duration;
    while (remaining > 0ms)
    {
        if (t.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled);
        }
        const auto step = remaining < slice ? remaining : slice;
        QThread::msleep(fastecu::saturating_ms<unsigned long>(step));
        remaining -= step;
    }
    return {};
}
