#pragma once
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/scripted_flash_transport_state.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fastecu::flash
{

// Framed-message fake (queued Bytes, not CanFrame(id,payload) pairs) --
// ICanFlashTransport::write/read already carry raw framed bytes, unlike
// cdbg::ICanTransport's per-frame id+payload shape. Modeled on
// ScriptedKlineFlashTransport (src/backend/flash/testing/scripted_kline_flash_transport.h).
class ScriptedCanFlashTransport : public ICanFlashTransport
{
  public:
    enum class OperationKind
    {
        kWrite,
        kRead
    };
    struct OperationAttempt
    {
        OperationKind kind;
        bool accepted;
        std::optional<ErrorKind> error;
        bool operator==(const OperationAttempt&) const = default;
    };

    const std::vector<OperationAttempt>& OperationAttempts() const
    {
        return operation_attempts_;
    }
    const std::optional<std::string>& FirstUnexpectedOperation() const
    {
        return first_unexpected_operation_;
    }

    ScriptedCanFlashTransport() = default;

    explicit ScriptedCanFlashTransport(ScriptedTransportInitialState initial_state)
        : open_(initial_state == ScriptedTransportInitialState::kOpen)
    {
    }

    bool IsOpen() const noexcept
    {
        return open_;
    }

    // RAII label for every step recorded while it is alive. One line per
    // script helper, so the label costs nothing per exchange and comes from a
    // function that is already named.
    class ScriptSection
    {
      public:
        ScriptSection(ScriptedCanFlashTransport& transport, std::string_view label) : transport_(&transport)
        {
            previous_ = transport.current_section_;
            transport.current_section_ = std::string(label);
        }
        ScriptSection(const ScriptSection&) = delete;
        ScriptSection& operator=(const ScriptSection&) = delete;
        ~ScriptSection()
        {
            transport_->current_section_ = previous_;
        }

      private:
        ScriptedCanFlashTransport *transport_;
        std::string previous_;
    };

    [[nodiscard]] ScriptSection Section(std::string_view label)
    {
        return ScriptSection(*this, label);
    }

    void Exchange(bytes::ByteView request, bytes::ByteView response)
    {
        expected_.emplace_back(request.begin(), request.end());
        sections_.emplace_back(current_section_);
        reads_.emplace_back(std::optional<bytes::Bytes>{bytes::Bytes(response.begin(), response.end())});
    }

    void Exchange(bytes::ByteView request)
    {
        expected_.emplace_back(request.begin(), request.end());
        sections_.emplace_back(current_section_);
    }

    void ExpectWrite(bytes::ByteView b)
    {
        expected_.emplace_back(b.begin(), b.end());
        sections_.emplace_back(current_section_);
    }
    void QueueRead(bytes::ByteView b)
    {
        reads_.emplace_back(std::optional<bytes::Bytes>{bytes::Bytes(b.begin(), b.end())});
    }
    void QueueNoFrame()
    {
        reads_.emplace_back(std::optional<bytes::Bytes>{});
    }
    void QueueError(ErrorKind kind, std::string detail = {})
    {
        reads_.emplace_back(Fail(kind, std::move(detail)));
    }
    void QueueBlockingRead()
    {
        std::lock_guard lock(mutex_);
        blocking_read_pending_ = true;
    }
    bool ScriptConsumed() const
    {
        return w_idx_ == expected_.size() && reads_.empty() && !blocking_read_pending_;
    }
    std::size_t WritesConsumed() const
    {
        return w_idx_;
    }
    // Every timeout read() was called with, in order. Lets a test pin a
    // family's wire timing, which otherwise leaves no trace in the script --
    // the four Denso ISO-15765 families deliberately differ here.
    const std::vector<std::chrono::milliseconds>& ReadTimeouts() const
    {
        return read_timeouts_;
    }

    Status ResetConnection() override
    {
        lifecycle_calls.push_back("reset_connection");
        ++reset_call_count;
        open_ = false;
        return reset_result;
    }

    Status Configure(const Iso15765Config& config) override
    {
        lifecycle_calls.push_back("configure");
        ++configure_call_count;
        last_config = config;
        return configure_result;
    }
    Status Open() override
    {
        lifecycle_calls.push_back("open");
        ++open_call_count;
        open_ = true;
        return open_result;
    }
    Status Close() override
    {
        lifecycle_calls.push_back("close");
        ++close_call_count;
        open_ = false;
        return close_result;
    }
    void RequestUnblock() noexcept override
    {
        std::lock_guard lock(mutex_);
        unblock_requested_ = true;
        cv_.notify_all();
    }
    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        operation_attempts_.push_back({OperationKind::kWrite, false, std::nullopt});
        if (cancellation.Cancelled())
        {
            operation_attempts_.back().error = ErrorKind::kCancelled;
            return Fail(ErrorKind::kCancelled, "scripted CAN write cancelled");
        }
        if (w_idx_ >= expected_.size())
        {
            return UnexpectedOperation(
                std::format("scripted CAN write ran past the end of the script ({} exchanges); wrote {}",
                            expected_.size(), bytes::ToHex(data)));
        }
        const auto& expected = expected_.at(w_idx_);
        if (!std::equal(expected.begin(), expected.end(), data.begin(), data.end()))
        {
            return UnexpectedOperation(DescribeDivergence(w_idx_, data));
        }
        ++w_idx_;
        operation_attempts_.back().accepted = true;
        return {};
    }
    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        operation_attempts_.push_back({OperationKind::kRead, false, std::nullopt});
        read_timeouts_.push_back(timeout);
        {
            std::unique_lock lock(mutex_);
            if (blocking_read_pending_)
            {
                cv_.wait(lock, [this] { return unblock_requested_; });
                blocking_read_pending_ = false;
                operation_attempts_.back().accepted = true;
                operation_attempts_.back().error = ErrorKind::kCancelled;
                return Fail(ErrorKind::kCancelled, "scripted CAN read unblocked");
            }
        }
        if (cancellation.Cancelled())
        {
            operation_attempts_.back().error = ErrorKind::kCancelled;
            return Fail(ErrorKind::kCancelled, "scripted CAN read cancelled");
        }
        if (reads_.empty())
        {
            return UnexpectedOperation("no scripted CAN read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        operation_attempts_.back().accepted = true;
        if (!result.has_value())
        {
            operation_attempts_.back().error = result.error().kind;
        }
        return result;
    }

    int reset_call_count = 0;
    int configure_call_count = 0;
    int open_call_count = 0;
    int close_call_count = 0;
    std::vector<std::string> lifecycle_calls;
    Status reset_result;
    Status configure_result;
    Status open_result;
    Status close_result;
    std::optional<Iso15765Config> last_config;

  private:
    std::unexpected<Error> UnexpectedOperation(std::string detail)
    {
        operation_attempts_.back().error = ErrorKind::kInternal;
        if (!first_unexpected_operation_.has_value())
        {
            first_unexpected_operation_ = detail;
        }
        return Fail(ErrorKind::kInternal, std::move(detail));
    }

    std::vector<OperationAttempt> operation_attempts_;
    std::optional<std::string> first_unexpected_operation_;

    std::string DescribeDivergence(std::size_t index, bytes::ByteView actual) const
    {
        const std::string& label = sections_.at(index);
        const std::string where = label.empty() ? std::format("scripted CAN exchange #{}", index + 1)
                                                : std::format("scripted CAN exchange #{} (\"{}\")", index + 1, label);
        return std::format("{} diverged\n  expected: {}\n  actual:   {}", where, bytes::ToHex(expected_.at(index)),
                           bytes::ToHex(actual));
    }

    std::vector<bytes::Bytes> expected_;
    std::vector<std::string> sections_;
    std::string current_section_;
    std::deque<Result<std::optional<bytes::Bytes>>> reads_;
    std::size_t w_idx_ = 0;
    std::vector<std::chrono::milliseconds> read_timeouts_;
    bool open_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool blocking_read_pending_ = false;
    bool unblock_requested_ = false;
};

} // namespace fastecu::flash
