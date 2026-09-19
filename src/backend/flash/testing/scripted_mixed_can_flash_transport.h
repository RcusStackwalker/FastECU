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
    Unconfigured,
    Iso15765Kernel,
    RawBootloader,
    Closed,
};

class ScriptedMixedCanFlashTransport final : public IMixedCanFlashTransport
{
  public:
    void expectIsoWrite(bytes::Bytes data)
    {
        expected_iso_writes_.push_back(std::move(data));
    }
    void expectIsoWrite(bytes::ByteView data)
    {
        expected_iso_writes_.emplace_back(data.begin(), data.end());
    }
    void queueIsoRead(bytes::Bytes data)
    {
        iso_reads_.emplace_back(std::optional<bytes::Bytes>{std::move(data)});
    }
    void queueIsoRead(bytes::ByteView data)
    {
        iso_reads_.emplace_back(std::optional<bytes::Bytes>{bytes::Bytes(data.begin(), data.end())});
    }
    void expectRawWrite(cdbg::CanFrame frame)
    {
        expected_raw_writes_.push_back(std::move(frame));
    }
    void queueRawRead(cdbg::CanFrame frame)
    {
        raw_reads_.emplace_back(std::optional<cdbg::CanFrame>{std::move(frame)});
    }
    void queueNoIsoFrame()
    {
        iso_reads_.emplace_back(std::optional<bytes::Bytes>{});
    }
    void queueNoRawFrame()
    {
        raw_reads_.emplace_back(std::optional<cdbg::CanFrame>{});
    }
    void queueIsoError(ErrorKind kind, std::string detail = {})
    {
        iso_reads_.emplace_back(fail(kind, std::move(detail)));
    }
    void queueRawError(ErrorKind kind, std::string detail = {})
    {
        raw_reads_.emplace_back(fail(kind, std::move(detail)));
    }
    void queueBlockingIsoRead()
    {
        std::lock_guard lock(mutex_);
        blocking_iso_read_pending_ = true;
    }
    void queueBlockingRawRead()
    {
        std::lock_guard lock(mutex_);
        blocking_raw_read_pending_ = true;
    }
    bool waitUntilBlockingIsoReadEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return blocking_iso_read_entered_cv_.wait_for(lock, timeout, [this] { return blocking_iso_read_entered_; });
    }
    bool waitUntilBlockingRawReadEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return blocking_raw_read_entered_cv_.wait_for(lock, timeout, [this] { return blocking_raw_read_entered_; });
    }
    void failNextRawTransition()
    {
        fail_next_raw_transition_ = true;
    }
    void failNextIsoTransition()
    {
        fail_next_iso_transition_ = true;
    }
    bool scriptConsumed() const
    {
        return iso_write_index_ == expected_iso_writes_.size() && raw_write_index_ == expected_raw_writes_.size() &&
               iso_reads_.empty() && raw_reads_.empty() && !blocking_iso_read_pending_ && !blocking_raw_read_pending_;
    }
    const std::vector<ScriptedMixedCanMode>& modeChanges() const noexcept
    {
        return mode_changes_;
    }
    int configureCallCount() const noexcept
    {
        return configure_call_count_;
    }
    int openCallCount() const noexcept
    {
        return open_call_count_;
    }
    int closeCallCount() const noexcept
    {
        return close_call_count_;
    }

    Status reset_connection() override
    {
        ++reset_connection_call_count_;
        return reset_connection_result_;
    }

    Status configure(const MixedCanConfig& config) override
    {
        ++configure_call_count_;
        last_config_ = config;
        if (!configure_result_.has_value())
        {
            return configure_result_;
        }
        configured_ = true;
        mode_ = ScriptedMixedCanMode::Unconfigured;
        return {};
    }
    Status open() override
    {
        ++open_call_count_;
        if (!configured_)
        {
            return fail(ErrorKind::InvalidConfig, "scripted mixed CAN transport opened before configuration");
        }
        if (!open_result_.has_value())
        {
            return open_result_;
        }
        set_mode(ScriptedMixedCanMode::Iso15765Kernel);
        return {};
    }
    Status close() override
    {
        ++close_call_count_;
        if (!close_result_.has_value())
        {
            return close_result_;
        }
        set_mode(ScriptedMixedCanMode::Closed);
        return {};
    }
    Status enter_raw_bootloader_mode() override
    {
        if (mode_ != ScriptedMixedCanMode::Iso15765Kernel)
        {
            return wrong_mode("raw bootloader transition");
        }
        if (std::exchange(fail_next_raw_transition_, false))
        {
            return fail(ErrorKind::Internal, "scripted raw transition failed");
        }
        set_mode(ScriptedMixedCanMode::RawBootloader);
        return {};
    }
    Status clear_receive_buffer() override
    {
        if (mode_ != ScriptedMixedCanMode::RawBootloader)
        {
            return wrong_mode("receive-buffer clear");
        }
        ++clear_receive_buffer_call_count_;
        return {};
    }
    Status enter_iso15765_kernel_mode() override
    {
        if (mode_ != ScriptedMixedCanMode::RawBootloader)
        {
            return wrong_mode("ISO-15765 kernel transition");
        }
        if (std::exchange(fail_next_iso_transition_, false))
        {
            return fail(ErrorKind::Internal, "scripted ISO-15765 transition failed");
        }
        set_mode(ScriptedMixedCanMode::Iso15765Kernel);
        return {};
    }
    Status write_iso15765(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        if (cancelled_or_unblocked(cancellation))
        {
            return fail(ErrorKind::Cancelled, "scripted ISO-15765 write cancelled");
        }
        if (mode_ != ScriptedMixedCanMode::Iso15765Kernel)
        {
            return wrong_mode("ISO-15765 write");
        }
        if (iso_write_index_ >= expected_iso_writes_.size() ||
            expected_iso_writes_[iso_write_index_] != bytes::Bytes(data.begin(), data.end()))
        {
            return fail(ErrorKind::Internal, "unexpected scripted ISO-15765 write");
        }
        ++iso_write_index_;
        return {};
    }
    Result<std::optional<bytes::Bytes>> read_iso15765(std::chrono::milliseconds,
                                                      const ICancellationToken& cancellation) override
    {
        if (cancelled_or_unblocked(cancellation))
        {
            return fail(ErrorKind::Cancelled, "scripted ISO-15765 read cancelled");
        }
        if (mode_ != ScriptedMixedCanMode::Iso15765Kernel)
        {
            return std::unexpected(wrong_mode("ISO-15765 read").error());
        }
        if (const auto blocked = release_blocking_iso_read(cancellation); !blocked.has_value())
        {
            return std::unexpected(blocked.error());
        }
        if (iso_reads_.empty())
        {
            return fail(ErrorKind::Internal, "no scripted ISO-15765 read outcome");
        }
        auto result = std::move(iso_reads_.front());
        iso_reads_.pop_front();
        return result;
    }
    Status write_raw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation) override
    {
        if (cancelled_or_unblocked(cancellation))
        {
            return fail(ErrorKind::Cancelled, "scripted raw CAN write cancelled");
        }
        if (mode_ != ScriptedMixedCanMode::RawBootloader)
        {
            return wrong_mode("raw CAN write");
        }
        if (frame.payload.size() > 8)
        {
            return fail(ErrorKind::InvalidConfig, "raw CAN payload exceeds eight bytes");
        }
        if (raw_write_index_ >= expected_raw_writes_.size() ||
            !same_frame(expected_raw_writes_[raw_write_index_], frame))
        {
            return fail(ErrorKind::Internal, "unexpected scripted raw CAN write");
        }
        ++raw_write_index_;
        return {};
    }
    Result<std::optional<cdbg::CanFrame>> read_raw(std::chrono::milliseconds,
                                                   const ICancellationToken& cancellation) override
    {
        if (cancelled_or_unblocked(cancellation))
        {
            return fail(ErrorKind::Cancelled, "scripted raw CAN read cancelled");
        }
        if (mode_ != ScriptedMixedCanMode::RawBootloader)
        {
            return std::unexpected(wrong_mode("raw CAN read").error());
        }
        if (const auto blocked = release_blocking_raw_read(cancellation); !blocked.has_value())
        {
            return std::unexpected(blocked.error());
        }
        if (raw_reads_.empty())
        {
            return fail(ErrorKind::Internal, "no scripted raw CAN read outcome");
        }
        auto result = std::move(raw_reads_.front());
        raw_reads_.pop_front();
        if (result.has_value() && result->has_value() && result->value().payload.size() > 8)
        {
            return fail(ErrorKind::InvalidConfig, "scripted raw CAN frame exceeds eight bytes");
        }
        return result;
    }
    void request_unblock() noexcept override
    {
        std::lock_guard lock(mutex_);
        unblock_requested_ = true;
        blocking_iso_read_cv_.notify_all();
        blocking_raw_read_cv_.notify_all();
    }

    int reset_connection_call_count_ = 0;
    Status reset_connection_result_;
    Status configure_result_;
    Status open_result_;
    Status close_result_;
    std::optional<MixedCanConfig> last_config_;
    int clear_receive_buffer_call_count_ = 0;

  private:
    static bool same_frame(const cdbg::CanFrame& left, const cdbg::CanFrame& right)
    {
        return left.id == right.id && left.payload == right.payload;
    }
    Status wrong_mode(std::string_view operation) const
    {
        return fail(ErrorKind::InvalidConfig, std::format("scripted mixed CAN {} in wrong mode", operation));
    }
    void set_mode(ScriptedMixedCanMode mode)
    {
        mode_ = mode;
        mode_changes_.push_back(mode);
    }
    bool cancelled_or_unblocked(const ICancellationToken& cancellation) const
    {
        {
            std::lock_guard lock(mutex_);
            if (unblock_requested_)
            {
                return true;
            }
        }
        return cancellation.cancelled();
    }
    Status release_blocking_iso_read(const ICancellationToken&)
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
        return fail(ErrorKind::Cancelled, "scripted ISO-15765 read unblocked");
    }
    Status release_blocking_raw_read(const ICancellationToken&)
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
        return fail(ErrorKind::Cancelled, "scripted raw CAN read unblocked");
    }

    std::vector<bytes::Bytes> expected_iso_writes_;
    std::deque<Result<std::optional<bytes::Bytes>>> iso_reads_;
    std::size_t iso_write_index_ = 0;
    std::vector<cdbg::CanFrame> expected_raw_writes_;
    std::deque<Result<std::optional<cdbg::CanFrame>>> raw_reads_;
    std::size_t raw_write_index_ = 0;
    ScriptedMixedCanMode mode_ = ScriptedMixedCanMode::Unconfigured;
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
