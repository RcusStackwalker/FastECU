#pragma once
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/scripted_flash_transport_state.h"

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
    ScriptedCanFlashTransport() = default;

    explicit ScriptedCanFlashTransport(ScriptedTransportInitialState initial_state)
        : open_(initial_state == ScriptedTransportInitialState::Open)
    {
    }

    bool is_open() const noexcept
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

    [[nodiscard]] ScriptSection section(std::string_view label)
    {
        return ScriptSection(*this, label);
    }

    void exchange(bytes::ByteView request, bytes::ByteView response)
    {
        expected_.emplace_back(request.begin(), request.end());
        sections_.emplace_back(current_section_);
        reads_.emplace_back(std::optional<bytes::Bytes>{bytes::Bytes(response.begin(), response.end())});
    }

    void exchange(bytes::ByteView request)
    {
        expected_.emplace_back(request.begin(), request.end());
        sections_.emplace_back(current_section_);
    }

    void expectWrite(bytes::ByteView b)
    {
        expected_.emplace_back(b.begin(), b.end());
        sections_.emplace_back(current_section_);
    }
    void queueRead(bytes::ByteView b)
    {
        reads_.emplace_back(std::optional<bytes::Bytes>{bytes::Bytes(b.begin(), b.end())});
    }
    void queue_no_frame()
    {
        reads_.emplace_back(std::optional<bytes::Bytes>{});
    }
    void queue_error(ErrorKind kind, std::string detail = {})
    {
        reads_.emplace_back(fail(kind, std::move(detail)));
    }
    void queueBlockingRead()
    {
        std::lock_guard lock(mutex_);
        blocking_read_pending_ = true;
    }
    bool scriptConsumed() const
    {
        return wIdx_ == expected_.size() && reads_.empty() && !blocking_read_pending_;
    }
    std::size_t writesConsumed() const
    {
        return wIdx_;
    }
    // Every timeout read() was called with, in order. Lets a test pin a
    // family's wire timing, which otherwise leaves no trace in the script --
    // the four Denso ISO-15765 families deliberately differ here.
    const std::vector<std::chrono::milliseconds>& readTimeouts() const
    {
        return read_timeouts_;
    }

    Status configure(const Iso15765Config& config) override
    {
        last_config_ = config;
        return configure_result_;
    }
    Status open() override
    {
        open_ = true;
        return open_result_;
    }
    Status close() override
    {
        ++close_call_count_;
        open_ = false;
        return close_result_;
    }
    void request_unblock() noexcept override
    {
        std::lock_guard lock(mutex_);
        unblock_requested_ = true;
        cv_.notify_all();
    }
    Status write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "scripted CAN write cancelled");
        }
        const bytes::Bytes actual(data.begin(), data.end());
        if (wIdx_ >= expected_.size())
        {
            return fail(ErrorKind::Internal,
                        std::format("scripted CAN write ran past the end of the script ({} exchanges); wrote {}",
                                    expected_.size(), bytes::toHex(actual)));
        }
        if (expected_.at(wIdx_) != actual)
        {
            return fail(ErrorKind::Internal, describeDivergence(wIdx_, actual));
        }
        ++wIdx_;
        return {};
    }
    Result<std::optional<bytes::Bytes>> read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        read_timeouts_.push_back(timeout);
        {
            std::unique_lock lock(mutex_);
            if (blocking_read_pending_)
            {
                cv_.wait(lock, [this] { return unblock_requested_; });
                blocking_read_pending_ = false;
                return fail(ErrorKind::Cancelled, "scripted CAN read unblocked");
            }
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "scripted CAN read cancelled");
        }
        if (reads_.empty())
        {
            return fail(ErrorKind::Internal, "no scripted CAN read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        return result;
    }

    int close_call_count_ = 0;
    Status configure_result_;
    Status open_result_;
    Status close_result_;
    std::optional<Iso15765Config> last_config_;

  private:
    std::string describeDivergence(std::size_t index, const bytes::Bytes& actual) const
    {
        const std::string& label = sections_.at(index);
        const std::string where = label.empty() ? std::format("scripted CAN exchange #{}", index + 1)
                                                : std::format("scripted CAN exchange #{} (\"{}\")", index + 1, label);
        return std::format("{} diverged\n  expected: {}\n  actual:   {}", where, bytes::toHex(expected_.at(index)),
                           bytes::toHex(actual));
    }

    std::vector<bytes::Bytes> expected_;
    std::vector<std::string> sections_;
    std::string current_section_;
    std::deque<Result<std::optional<bytes::Bytes>>> reads_;
    std::size_t wIdx_ = 0;
    std::vector<std::chrono::milliseconds> read_timeouts_;
    bool open_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool blocking_read_pending_ = false;
    bool unblock_requested_ = false;
};

} // namespace fastecu::flash
