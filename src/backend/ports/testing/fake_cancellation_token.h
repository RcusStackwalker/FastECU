#pragma once

#include "src/backend/ports/cancellation.h"

#include <cstddef>
#include <functional>
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
        cancelled_ = cancelled;
    }

    void cancel_on_check(std::size_t one_based_check)
    {
        cancel_on_check_ = one_based_check;
    }

    void set_predicate(std::function<bool()> predicate)
    {
        predicate_ = std::move(predicate);
    }

    std::size_t check_count() const
    {
        return check_count_;
    }

    bool cancelled() const override
    {
        ++check_count_;
        if (predicate_)
        {
            return predicate_();
        }
        if (cancel_on_check_)
        {
            return check_count_ >= *cancel_on_check_;
        }
        return cancelled_;
    }

  private:
    bool cancelled_;
    std::optional<std::size_t> cancel_on_check_;
    std::function<bool()> predicate_;
    mutable std::size_t check_count_ = 0;
};

} // namespace fastecu
