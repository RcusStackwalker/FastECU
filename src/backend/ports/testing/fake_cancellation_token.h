#pragma once

#include "src/backend/ports/cancellation.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace fastecu
{

class FakeCancellationToken final : public ICancellationToken
{
  public:
    explicit FakeCancellationToken(bool cancelled = false) : cancelled_(cancelled)
    {
    }

    void set_cancelled(bool cancelled)
    {
        std::lock_guard lock(mutex_);
        cancelled_ = cancelled;
    }

    void cancel_on_check(std::size_t one_based_check)
    {
        std::lock_guard lock(mutex_);
        cancel_on_check_ = one_based_check;
    }

    void set_predicate(std::function<bool()> predicate)
    {
        std::lock_guard lock(mutex_);
        predicate_ = std::move(predicate);
    }

    std::size_t check_count() const
    {
        std::lock_guard lock(mutex_);
        return check_count_;
    }

    bool cancelled() const override
    {
        std::function<bool()> predicate;
        std::optional<std::size_t> cancel_on_check;
        bool cancelled;
        std::size_t check_count;
        {
            std::lock_guard lock(mutex_);
            check_count = ++check_count_;
            predicate = predicate_;
            cancel_on_check = cancel_on_check_;
            cancelled = cancelled_;
        }
        if (predicate)
        {
            return predicate();
        }
        if (cancel_on_check)
        {
            return check_count >= *cancel_on_check;
        }
        return cancelled;
    }

  private:
    mutable std::mutex mutex_;
    bool cancelled_;
    std::optional<std::size_t> cancel_on_check_;
    std::function<bool()> predicate_;
    mutable std::size_t check_count_ = 0;
};

} // namespace fastecu
