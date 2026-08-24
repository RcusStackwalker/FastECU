#pragma once
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/scripted_flash_transport_state.h"

#include <condition_variable>
#include <deque>
#include <mutex>
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

    void expectWrite(bytes::ByteView b)
    {
        expected_.emplace_back(b.begin(), b.end());
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
    // For tests that drive an executor directly. Executors no longer open
    // their transport (ADR 0015), so the fake must start in the state a
    // BoundAttempt would have left it in.
    void start_open()
    {
        open_ = true;
    }
    bool scriptConsumed() const
    {
        return wIdx_ == expected_.size() && reads_.empty() && !blocking_read_pending_;
    }
    std::size_t writesConsumed() const
    {
        return wIdx_;
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
        if (wIdx_ >= expected_.size() || expected_.at(wIdx_) != bytes::Bytes(data.begin(), data.end()))
        {
            return fail(ErrorKind::Internal, "unexpected scripted CAN write");
        }
        ++wIdx_;
        return {};
    }
    Result<std::optional<bytes::Bytes>> read(int, const ICancellationToken& cancellation) override
    {
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
    std::vector<bytes::Bytes> expected_;
    std::deque<Result<std::optional<bytes::Bytes>>> reads_;
    std::size_t wIdx_ = 0;
    bool open_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool blocking_read_pending_ = false;
    bool unblock_requested_ = false;
};

} // namespace fastecu::flash
