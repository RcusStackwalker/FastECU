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
    ScriptedKlineFlashTransport() = default;

    explicit ScriptedKlineFlashTransport(ScriptedTransportInitialState initial_state)
        : open_(initial_state == ScriptedTransportInitialState::kOpen)
    {
    }

    enum class ControlLineAction
    {
        kDisableLecLines,
        kPulseLec2,
        kEnableProgrammingVoltageLine,
        kEnableBootModeLines,
    };
    enum class Operation
    {
        kDisableLecLines,
        kPulseLec2,
        kEnableProgrammingVoltageLine,
        kEnableBootModeLines,
        kRead10,
    };

    // RAII label for every step recorded while it is alive. One line per
    // script helper, so the label costs nothing per exchange and comes from a
    // function that is already named.
    class ScriptSection
    {
      public:
        ScriptSection(ScriptedKlineFlashTransport& transport, std::string_view label) : transport_(&transport)
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
        ScriptedKlineFlashTransport *transport_;
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
        ++reads_queued_;
        reads_.emplace_back(OptionalBytes{bytes::Bytes(response.begin(), response.end())});
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
    void ExpectRawWrite(bytes::ByteView b)
    {
        ExpectWrite(b);
        raw_writes_.push_back(expected_.size() - 1);
    }
    void QueueRawRead(bytes::ByteView b)
    {
        raw_reads_.push_back(reads_queued_++);
        reads_.emplace_back(OptionalBytes{bytes::Bytes(b.begin(), b.end())});
    }
    void QueueRead(bytes::ByteView b)
    {
        ++reads_queued_;
        reads_.emplace_back(OptionalBytes{bytes::Bytes(b.begin(), b.end())});
    }
    void QueueNoFrame()
    {
        ++reads_queued_;
        reads_.emplace_back(OptionalBytes{});
    }
    void QueueError(ErrorKind kind, std::string detail = {})
    {
        ++reads_queued_;
        reads_.emplace_back(Fail(kind, std::move(detail)));
    }
    void QueueBlockingRead()
    {
        std::lock_guard lock(mutex_);
        blocking_read_pending_ = true;
    }
    // Blocks until read() has actually entered the queued blocking read.
    // Tests use this instead of a wall-clock sleep to reach the
    // "transport is mid-read" state before calling requestStop(), so what
    // they prove about unblocking does not depend on the worker thread
    // winning a race against a fixed sleep. Mirrors
    // ScriptedLoggingProtocol::waitUntilPollEntered.
    bool WaitUntilBlockingReadEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return blocking_read_entered_cv_.wait_for(lock, timeout, [this] { return blocking_read_entered_; });
    }
    bool ScriptConsumed() const
    {
        return w_idx_ == expected_.size() && reads_.empty() && !blocking_read_pending_;
    }
    std::size_t WritesConsumed() const
    {
        return w_idx_;
    }

    Status ResetConnection() override
    {
        lifecycle_calls.push_back("reset_connection");
        ++reset_call_count;
        open_ = false;
        return reset_result;
    }

    Status Configure(const KlineConfig& config) override
    {
        lifecycle_calls.push_back("configure");
        last_config = config;
        return configure_result;
    }
    Status Open() override
    {
        lifecycle_calls.push_back("open");
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
    Status DisableLecLines() override
    {
        control_line_trace.push_back(ControlLineAction::kDisableLecLines);
        operation_trace.push_back(Operation::kDisableLecLines);
        return disable_lec_lines_result;
    }
    Status PulseLec2Line(std::chrono::milliseconds timeout) override
    {
        control_line_trace.push_back(ControlLineAction::kPulseLec2);
        operation_trace.push_back(Operation::kPulseLec2);
        lec_2_pulse_timeouts.push_back(timeout);
        return pulse_lec_2_line_result;
    }
    Status EnableProgrammingVoltageLine() override
    {
        programming_voltage_line_write_index = w_idx_;
        control_line_trace.push_back(ControlLineAction::kEnableProgrammingVoltageLine);
        operation_trace.push_back(Operation::kEnableProgrammingVoltageLine);
        return enable_programming_voltage_line_result;
    }
    Status EnableBootModeLines() override
    {
        control_line_trace.push_back(ControlLineAction::kEnableBootModeLines);
        operation_trace.push_back(Operation::kEnableBootModeLines);
        return enable_boot_mode_lines_result;
    }
    bool RequiresPostKernelUploadDelay() const override
    {
        return post_kernel_upload_delay_required;
    }
    Status SetAddIso14230Header(bool add_header) override
    {
        header_mode_calls.push_back(add_header);
        return set_add_iso14230_header_result;
    }
    void RequestUnblock() noexcept override
    {
        std::lock_guard lock(mutex_);
        unblock_requested_ = true;
        cv_.notify_all();
    }
    bool IsOpen() const override
    {
        return open_;
    }
    Status SetBaud(int baud) override
    {
        baud_calls.push_back(baud);
        return set_baud_result;
    }
    Result<std::size_t> Write(bytes::ByteView data) override
    {
        const bytes::Bytes actual(data.begin(), data.end());
        if (w_idx_ >= expected_.size())
        {
            return Fail(ErrorKind::kInternal,
                        std::format("scripted K-Line write ran past the end of the script ({} exchanges); wrote {}",
                                    expected_.size(), bytes::ToHex(actual)));
        }
        if (expected_.at(w_idx_) != actual)
        {
            return Fail(ErrorKind::kInternal, DescribeDivergence(w_idx_, actual));
        }
        if (std::find(raw_writes_.begin(), raw_writes_.end(), w_idx_) != raw_writes_.end())
        {
            return Fail(ErrorKind::kInternal, "expected raw K-Line write");
        }
        ++w_idx_;
        return data.size();
    }
    Result<std::size_t> WriteRaw(bytes::ByteView data) override
    {
        const bytes::Bytes actual(data.begin(), data.end());
        if (w_idx_ >= expected_.size() || expected_[w_idx_] != actual ||
            std::find(raw_writes_.begin(), raw_writes_.end(), w_idx_) == raw_writes_.end())
        {
            return Fail(ErrorKind::kInternal, "unexpected raw K-Line write");
        }
        ++w_idx_;
        return data.size();
    }
    Result<OptionalBytes> ReadRaw(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (std::find(raw_reads_.begin(), raw_reads_.end(), reads_consumed_) == raw_reads_.end())
        {
            return Fail(ErrorKind::kInternal, "unexpected raw K-Line read");
        }
        return ReadImpl(timeout, cancellation);
    }
    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (std::find(raw_reads_.begin(), raw_reads_.end(), reads_consumed_) != raw_reads_.end())
        {
            return Fail(ErrorKind::kInternal, "expected raw K-Line read");
        }
        return ReadImpl(timeout, cancellation);
    }
    Result<OptionalBytes> ReadImpl(std::chrono::milliseconds timeout, const ICancellationToken& cancellation)
    {
        read_timeouts.push_back(timeout);
        if (timeout == std::chrono::milliseconds{10})
        {
            operation_trace.push_back(Operation::kRead10);
        }
        {
            std::unique_lock lock(mutex_);
            if (blocking_read_pending_)
            {
                blocking_read_entered_ = true;
                blocking_read_entered_cv_.notify_all();
                cv_.wait(lock, [this] { return unblock_requested_; });
                blocking_read_pending_ = false;
                return Fail(ErrorKind::kCancelled, "scripted K-Line read unblocked");
            }
        }
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "scripted K-Line read cancelled");
        }
        if (reads_.empty())
        {
            return Fail(ErrorKind::kInternal, "no scripted K-Line read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        ++reads_consumed_;
        return result;
    }

    int close_call_count = 0;
    // Lifecycle calls in order, as ScriptedCanFlashTransport records them, so
    // a test can pin a reset before configure().
    std::vector<std::string> lifecycle_calls;
    int reset_call_count = 0;
    Status reset_result;
    Status configure_result;
    Status open_result;
    Status close_result;
    Status disable_lec_lines_result;
    Status pulse_lec_2_line_result;
    Status enable_programming_voltage_line_result;
    Status enable_boot_mode_lines_result;
    bool post_kernel_upload_delay_required = false;
    std::optional<KlineConfig> last_config;
    std::vector<ControlLineAction> control_line_trace;
    std::vector<std::chrono::milliseconds> lec_2_pulse_timeouts;
    std::vector<std::chrono::milliseconds> read_timeouts;
    std::vector<Operation> operation_trace;
    std::optional<std::size_t> programming_voltage_line_write_index;

    // Records every set_add_iso14230_header() call in order (true == "add
    // header", false == "don't") so tests can assert the exact transitions
    // relative to connect/upload/read -- see denso_sh705x_eeprom_kline_
    // executor.cpp's execute() for the call sites this proves.
    std::vector<bool> header_mode_calls;
    Status set_add_iso14230_header_result;
    std::vector<int> baud_calls;
    Status set_baud_result;

  private:
    std::string DescribeDivergence(std::size_t index, const bytes::Bytes& actual) const
    {
        const std::string& label = sections_.at(index);
        const std::string where = label.empty()
                                      ? std::format("scripted K-Line exchange #{}", index + 1)
                                      : std::format("scripted K-Line exchange #{} (\"{}\")", index + 1, label);
        return std::format("{} diverged\n  expected: {}\n  actual:   {}", where, bytes::ToHex(expected_.at(index)),
                           bytes::ToHex(actual));
    }

    std::vector<std::size_t> raw_writes_;
    std::vector<std::size_t> raw_reads_;
    std::size_t reads_consumed_ = 0;
    std::size_t reads_queued_ = 0;
    std::vector<bytes::Bytes> expected_;
    std::vector<std::string> sections_;
    std::string current_section_;
    std::deque<Result<OptionalBytes>> reads_;
    std::size_t w_idx_ = 0;
    bool open_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable blocking_read_entered_cv_;
    bool blocking_read_pending_ = false;
    bool blocking_read_entered_ = false;
    bool unblock_requested_ = false;
};

} // namespace fastecu::flash
