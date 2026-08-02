#pragma once

#include "src/backend/logging/logging_protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

// Portable protocol test double shared by the desktop worker and engine tests.
// A blocking poll observes cancellation so teardown has a deterministic bound.
class ScriptedLoggingProtocol final : public fastecu::logging::LoggingProtocol
{
  public:
    void queueStartResult(fastecu::Status result)
    {
        start_results_.push_back(std::move(result));
    }

    void queuePollResult(fastecu::Result<fastecu::logging::PollData> result)
    {
        poll_results_.push_back(std::move(result));
    }

    void blockPollUntilCancelled()
    {
        block_poll_.store(true, std::memory_order_relaxed);
    }

    bool waitUntilPollEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return poll_entered_cv_.wait_for(lock, timeout, [this]
                                         { return poll_entered_; });
    }

    int startCallCount() const
    {
        return start_calls_.load(std::memory_order_relaxed);
    }

    bool stopCalled() const
    {
        return stop_called_.load(std::memory_order_relaxed);
    }

    fastecu::Status start(const fastecu::ICancellationToken&) override
    {
        start_calls_.fetch_add(1, std::memory_order_relaxed);
        if (start_results_.empty())
        {
            return {};
        }
        auto result = std::move(start_results_.front());
        start_results_.pop_front();
        return result;
    }

    fastecu::Result<fastecu::logging::PollData> poll(
        int, const fastecu::ICancellationToken& cancellation) override
    {
        if (block_poll_.load(std::memory_order_relaxed))
        {
            {
                std::lock_guard lock(mutex_);
                poll_entered_ = true;
            }
            poll_entered_cv_.notify_all();
            while (!cancellation.cancelled())
            {
                std::unique_lock lock(mutex_);
                cancellation_poll_cv_.wait_for(lock, std::chrono::milliseconds(1));
            }
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted poll cancelled");
        }

        if (poll_results_.empty())
        {
            return fastecu::logging::PollData{.responded = false};
        }
        auto result = std::move(poll_results_.front());
        poll_results_.pop_front();
        return result;
    }

    fastecu::Status stop() override
    {
        stop_called_.store(true, std::memory_order_relaxed);
        return {};
    }

  private:
    std::deque<fastecu::Status> start_results_;
    std::deque<fastecu::Result<fastecu::logging::PollData>> poll_results_;
    std::atomic<int> start_calls_{0};
    std::atomic<bool> stop_called_{false};
    std::atomic<bool> block_poll_{false};
    std::mutex mutex_;
    std::condition_variable poll_entered_cv_;
    std::condition_variable cancellation_poll_cv_;
    bool poll_entered_ = false;
};
