#pragma once
#include "src/backend/flash/flash_executor.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace fastecu::flash
{

// Modeled on mutdma::ScriptedKlineTransport (tests/scripted_kline_transport.h)
// but implements the flash-specific IKlineFlashTransport surface (configure/
// open/close/request_unblock) in addition to the inherited setBaud/write/
// read/isOpen from mutdma::IKlineTransport. queueBlockingRead() supports the
// deterministic teardown test in Task 13 by making read() block on a
// condition variable until request_unblock() is called, with no wall-clock
// sleep on either side.
class ScriptedKlineFlashTransport : public IKlineFlashTransport
{
  public:
    void expectWrite(bytes::ByteView b)
    {
        expected_.emplace_back(b.begin(), b.end());
    }
    void queueRead(bytes::ByteView b)
    {
        reads_.emplace_back(OptionalBytes{bytes::Bytes(b.begin(), b.end())});
    }
    void queue_no_frame()
    {
        reads_.emplace_back(OptionalBytes{});
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

    Status configure(const KlineConfig& config) override
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
    Status set_add_iso14230_header(bool add_header) override
    {
        header_mode_calls_.push_back(add_header);
        return set_add_iso14230_header_result_;
    }
    void request_unblock() noexcept override
    {
        std::lock_guard lock(mutex_);
        unblock_requested_ = true;
        cv_.notify_all();
    }
    bool isOpen() const override
    {
        return open_;
    }
    Status setBaud(int) override
    {
        return {};
    }
    Result<std::size_t> write(bytes::ByteView data) override
    {
        if (wIdx_ >= expected_.size() || expected_.at(wIdx_) != bytes::Bytes(data.begin(), data.end()))
        {
            return fail(ErrorKind::Internal, "unexpected scripted K-Line write");
        }
        ++wIdx_;
        return data.size();
    }
    Result<OptionalBytes> read(int, const ICancellationToken& cancellation) override
    {
        {
            std::unique_lock lock(mutex_);
            if (blocking_read_pending_)
            {
                cv_.wait(lock, [this]
                         { return unblock_requested_; });
                blocking_read_pending_ = false;
                return fail(ErrorKind::Cancelled, "scripted K-Line read unblocked");
            }
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "scripted K-Line read cancelled");
        }
        if (reads_.empty())
        {
            return fail(ErrorKind::Internal, "no scripted K-Line read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        return result;
    }

    int close_call_count_ = 0;
    Status configure_result_;
    Status open_result_;
    Status close_result_;
    std::optional<KlineConfig> last_config_;

    // Records every set_add_iso14230_header() call in order (true == "add
    // header", false == "don't") so tests can assert the exact transitions
    // relative to connect/upload/read -- see denso_sh705x_eeprom_kline_
    // executor.cpp's execute() for the call sites this proves.
    std::vector<bool> header_mode_calls_;
    Status set_add_iso14230_header_result_;

  private:
    std::vector<bytes::Bytes> expected_;
    std::deque<Result<OptionalBytes>> reads_;
    std::size_t wIdx_ = 0;
    bool open_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool blocking_read_pending_ = false;
    bool unblock_requested_ = false;
};

} // namespace fastecu::flash
