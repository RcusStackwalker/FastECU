#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <chrono>
#include <thread>
#include <tuple>

namespace fastecu::testing
{
inline void ProcessPendingEvents()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

template <typename Predicate> bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        if (predicate())
        {
            return true;
        }
        ProcessPendingEvents();
        if (predicate())
        {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

inline void ProcessEventsFor(std::chrono::milliseconds duration)
{
    std::ignore = WaitUntil([] { return false; }, duration);
}
} // namespace fastecu::testing
