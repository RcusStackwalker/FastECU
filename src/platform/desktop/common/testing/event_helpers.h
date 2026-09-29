#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <chrono>
#include <thread>

namespace fastecu::testing
{
inline void process_pending_events()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

template <typename Predicate> bool wait_until(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        if (predicate())
            return true;
        process_pending_events();
        if (predicate())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

inline void process_events_for(std::chrono::milliseconds duration)
{
    (void)wait_until([] { return false; }, duration);
}
} // namespace fastecu::testing
