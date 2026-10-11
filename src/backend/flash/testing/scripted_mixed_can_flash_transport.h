#pragma once

#include "src/backend/flash/flash_executor.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastecu::flash
{

enum class ScriptedMixedCanMode
{
    kUnconfigured,
    kIso15765Kernel,
    kRawBootloader,
    kClosed,
};

class ScriptedMixedCanFlashTransport final : public IMixedCanFlashTransport
{
  public:
    void ExpectIsoWrite(bytes::Bytes data)
    {
        expected_iso_writes_.push_back(std::move(data));
    }
    void ExpectIsoWrite(bytes::ByteView data)
    {
        expected_iso_writes_.emplace_back(data.begin(), data.end());
    }
    void QueueIsoRead(bytes::Bytes data)
    {
        iso_reads_.emplace_back(std::optional<bytes::Bytes>{std::move(data)});
    }
    void QueueIsoRead(bytes::ByteView data)
    {
        iso_reads_.emplace_back(std::optional<bytes::Bytes>{bytes::Bytes(data.begin(), data.end())});
    }
    void ExpectRawWrite(cdbg::CanFrame frame)
    {
        expected_raw_writes_.push_back(std::move(frame));
    }
    void QueueRawRead(cdbg::CanFrame frame)
    {
        raw_reads_.emplace_back(std::optional<cdbg::CanFrame>{std::move(frame)});
    }
    void QueueNoIsoFrame()
    {
        iso_reads_.emplace_back(std::optional<bytes::Bytes>{});
    }
    void QueueNoRawFrame()
    {
        raw_reads_.emplace_back(std::optional<cdbg::CanFrame>{});
    }
    void QueueIsoError(ErrorKind kind, std::string detail = {})
    {
        iso_reads_.emplace_back(Fail(kind, std::move(detail)));
    }
    void QueueRawError(ErrorKind kind, std::string detail = {})
    {
        raw_reads_.emplace_back(Fail(kind, std::move(detail)));
    }
    void QueueBlockingIsoRead()
    {
        std::lock_guard lock(mutex_);
        blocking_iso_read_pending_ = true;
    }
    void QueueBlockingRawRead()
    {
        std::lock_guard lock(mutex_);
        blocking_raw_read_pending_ = true;
    }
    bool WaitUntilBlockingIsoReadEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return blocking_iso_read_entered_cv_.wait_for(lock, timeout, [this] { return blocking_iso_read_entered_; });
    }
    bool WaitUntilBlockingRawReadEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return blocking_raw_read_entered_cv_.wait_for(lock, timeout, [this] { return blocking_raw_read_entered_; });
    }
    void FailNextRawTransition()
    {
        fail_next_raw_transition_ = true;
    }
    void FailNextIsoTransition()
    {
        fail_next_iso_transition_ = true;
    }
    bool ScriptConsumed() const
    {
        return iso_write_index_ == expected_iso_writes_.size() && raw_write_index_ == expected_raw_writes_.size() &&
               iso_reads_.empty() && raw_reads_.empty() && !blocking_iso_read_pending_ && !blocking_raw_read_pending_;
    }
    const std::vector<ScriptedMixedCanMode>& ModeChanges() const noexcept
    {
        return mode_changes_;
    }
    int ConfigureCallCount() const noexcept
    {
        return configure_call_count_;
    }
    int OpenCallCount() const noexcept
    {
        return open_call_count_;
    }
    int CloseCallCount() const noexcept
    {
        return close_call_count_;
    }

    Status ResetConnection() override
    {
        ++reset_connection_call_count;
        return reset_connection_result;
    }

    Status Configure(const MixedCanConfig& config) override
    {
        ++configure_call_count_;
        last_config = config;
        if (!configure_result.has_value())
        {
            return configure_result;
        }
        configured_ = true;
        mode_ = ScriptedMixedCanMode::kUnconfigured;
        return {};
    }
    Status Open() override
    {
        ++open_call_count_;
        if (!configured_)
        {
            return Fail(ErrorKind::kInvalidConfig, "scripted mixed CAN transport opened before configuration");
        }
        if (!open_result.has_value())
        {
            return open_result;
        }
        SetMode(ScriptedMixedCanMode::kIso15765Kernel);
        return {};
    }
    Status Close() override
    {
        ++close_call_count_;
        if (!close_result.has_value())
        {
            return close_result;
        }
        SetMode(ScriptedMixedCanMode::kClosed);
        return {};
    }
    Status EnterRawBootloaderMode() override
    {
        if (mode_ != ScriptedMixedCanMode::kIso15765Kernel)
        {
            return WrongMode("raw bootloader transition");
        }
        if (std::exchange(fail_next_raw_transition_, false))
        {
            return Fail(ErrorKind::kInternal, "scripted raw transition failed");
        }
        SetMode(ScriptedMixedCanMode::kRawBootloader);
        return {};
    }
    Status ClearReceiveBuffer() override
    {
        if (mode_ != ScriptedMixedCanMode::kRawBootloader)
        {
            return WrongMode("receive-buffer clear");
        }
        ++clear_receive_buffer_call_count;
        return {};
    }
    Status EnterIso15765KernelMode() override
    {
        if (mode_ != ScriptedMixedCanMode::kRawBootloader)
        {
            return WrongMode("ISO-15765 kernel transition");
        }
        if (std::exchange(fail_next_iso_transition_, false))
        {
            return Fail(ErrorKind::kInternal, "scripted ISO-15765 transition failed");
        }
        SetMode(ScriptedMixedCanMode::kIso15765Kernel);
        return {};
    }
    Status WriteIso15765(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        if (CancelledOrUnblocked(cancellation))
        {
            return Fail(ErrorKind::kCancelled, "scripted ISO-15765 write cancelled");
        }
        if (mode_ != ScriptedMixedCanMode::kIso15765Kernel)
        {
            return WrongMode("ISO-15765 write");
        }
        if (iso_write_index_ >= expected_iso_writes_.size() ||
            expected_iso_writes_[iso_write_index_] != bytes::Bytes(data.begin(), data.end()))
        {
            return Fail(ErrorKind::kInternal, "unexpected scripted ISO-15765 write");
        }
        ++iso_write_index_;
        return {};
    }
    Result<std::optional<bytes::Bytes>> ReadIso15765(std::chrono::milliseconds,
                                                     const ICancellationToken& cancellation) override
    {
        if (CancelledOrUnblocked(cancellation))
        {
            return Fail(ErrorKind::kCancelled, "scripted ISO-15765 read cancelled");
        }
        if (mode_ != ScriptedMixedCanMode::kIso15765Kernel)
        {
            return std::unexpected(WrongMode("ISO-15765 read").error());
        }
        if (const auto blocked = ReleaseBlockingIsoRead(cancellation); !blocked.has_value())
        {
            return std::unexpected(blocked.error());
        }
        if (iso_reads_.empty())
        {
            return Fail(ErrorKind::kInternal, "no scripted ISO-15765 read outcome");
        }
        auto result = std::move(iso_reads_.front());
        iso_reads_.pop_front();
        return result;
    }
    Status WriteRaw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation) override
    {
        if (CancelledOrUnblocked(cancellation))
        {
            return Fail(ErrorKind::kCancelled, "scripted raw CAN write cancelled");
        }
        if (mode_ != ScriptedMixedCanMode::kRawBootloader)
        {
            return WrongMode("raw CAN write");
        }
        if (frame.payload.size() > 8)
        {
            return Fail(ErrorKind::kInvalidConfig, "raw CAN payload exceeds eight bytes");
        }
        if (raw_write_index_ >= expected_raw_writes_.size() ||
            !SameFrame(expected_raw_writes_[raw_write_index_], frame))
        {
            return Fail(ErrorKind::kInternal, "unexpected scripted raw CAN write");
        }
        ++raw_write_index_;
        return {};
    }
    Result<std::optional<cdbg::CanFrame>> ReadRaw(std::chrono::milliseconds,
                                                  const ICancellationToken& cancellation) override
    {
        if (CancelledOrUnblocked(cancellation))
        {
            return Fail(ErrorKind::kCancelled, "scripted raw CAN read cancelled");
        }
        if (mode_ != ScriptedMixedCanMode::kRawBootloader)
        {
            return std::unexpected(WrongMode("raw CAN read").error());
        }
        if (const auto blocked = ReleaseBlockingRawRead(cancellation); !blocked.has_value())
        {
            return std::unexpected(blocked.error());
        }
        if (raw_reads_.empty())
        {
            return Fail(ErrorKind::kInternal, "no scripted raw CAN read outcome");
        }
        auto result = std::move(raw_reads_.front());
        raw_reads_.pop_front();
        if (result.has_value() && result->has_value() && result->value().payload.size() > 8)
        {
            return Fail(ErrorKind::kInvalidConfig, "scripted raw CAN frame exceeds eight bytes");
        }
        return result;
    }
    void RequestUnblock() noexcept override
    {
        std::lock_guard lock(mutex_);
        unblock_requested_ = true;
        blocking_iso_read_cv_.notify_all();
        blocking_raw_read_cv_.notify_all();
    }

    int reset_connection_call_count = 0;
    Status reset_connection_result;
    Status configure_result;
    Status open_result;
    Status close_result;
    std::optional<MixedCanConfig> last_config;
    int clear_receive_buffer_call_count = 0;

  private:
    static bool SameFrame(const cdbg::CanFrame& left, const cdbg::CanFrame& right)
    {
        return left.id == right.id && left.payload == right.payload;
    }
    Status WrongMode(std::string_view operation) const
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("scripted mixed CAN {} in wrong mode", operation));
    }
    void SetMode(ScriptedMixedCanMode mode)
    {
        mode_ = mode;
        mode_changes_.push_back(mode);
    }
    bool CancelledOrUnblocked(const ICancellationToken& cancellation) const
    {
        {
            std::lock_guard lock(mutex_);
            if (unblock_requested_)
            {
                return true;
            }
        }
        return cancellation.Cancelled();
    }
    Status ReleaseBlockingIsoRead(const ICancellationToken&)
    {
        std::unique_lock lock(mutex_);
        if (!blocking_iso_read_pending_)
        {
            return {};
        }
        blocking_iso_read_entered_ = true;
        blocking_iso_read_entered_cv_.notify_all();
        blocking_iso_read_cv_.wait(lock, [this] { return unblock_requested_; });
        blocking_iso_read_pending_ = false;
        return Fail(ErrorKind::kCancelled, "scripted ISO-15765 read unblocked");
    }
    Status ReleaseBlockingRawRead(const ICancellationToken&)
    {
        std::unique_lock lock(mutex_);
        if (!blocking_raw_read_pending_)
        {
            return {};
        }
        blocking_raw_read_entered_ = true;
        blocking_raw_read_entered_cv_.notify_all();
        blocking_raw_read_cv_.wait(lock, [this] { return unblock_requested_; });
        blocking_raw_read_pending_ = false;
        return Fail(ErrorKind::kCancelled, "scripted raw CAN read unblocked");
    }

    std::vector<bytes::Bytes> expected_iso_writes_;
    std::deque<Result<std::optional<bytes::Bytes>>> iso_reads_;
    std::size_t iso_write_index_ = 0;
    std::vector<cdbg::CanFrame> expected_raw_writes_;
    std::deque<Result<std::optional<cdbg::CanFrame>>> raw_reads_;
    std::size_t raw_write_index_ = 0;
    ScriptedMixedCanMode mode_ = ScriptedMixedCanMode::kUnconfigured;
    std::vector<ScriptedMixedCanMode> mode_changes_;
    int configure_call_count_ = 0;
    int open_call_count_ = 0;
    int close_call_count_ = 0;
    bool configured_ = false;
    bool fail_next_raw_transition_ = false;
    bool fail_next_iso_transition_ = false;
    mutable std::mutex mutex_;
    std::condition_variable blocking_iso_read_cv_;
    std::condition_variable blocking_raw_read_cv_;
    std::condition_variable blocking_iso_read_entered_cv_;
    std::condition_variable blocking_raw_read_entered_cv_;
    bool blocking_iso_read_pending_ = false;
    bool blocking_raw_read_pending_ = false;
    bool blocking_iso_read_entered_ = false;
    bool blocking_raw_read_entered_ = false;
    bool unblock_requested_ = false;
};

} // namespace fastecu::flash
