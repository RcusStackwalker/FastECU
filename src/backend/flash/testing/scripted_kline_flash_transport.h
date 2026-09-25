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
        : open_(initial_state == ScriptedTransportInitialState::Open)
    {
    }

    enum class ControlLineAction
    {
        DisableLecLines,
        PulseLec2,
        EnableProgrammingVoltageLine,
        EnableBootModeLines,
    };
    enum class Operation
    {
        DisableLecLines,
        PulseLec2,
        EnableProgrammingVoltageLine,
        EnableBootModeLines,
        Read10,
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

    [[nodiscard]] ScriptSection section(std::string_view label)
    {
        return ScriptSection(*this, label);
    }

    void exchange(bytes::ByteView request, bytes::ByteView response)
    {
        expected_.emplace_back(request.begin(), request.end());
        sections_.emplace_back(current_section_);
        ++reads_queued_;
        reads_.emplace_back(OptionalBytes{bytes::Bytes(response.begin(), response.end())});
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
    void expectRawWrite(bytes::ByteView b)
    {
        expectWrite(b);
        raw_writes_.push_back(expected_.size() - 1);
    }
    void queueRawRead(bytes::ByteView b)
    {
        raw_reads_.push_back(reads_queued_++);
        reads_.emplace_back(OptionalBytes{bytes::Bytes(b.begin(), b.end())});
    }
    void queueRead(bytes::ByteView b)
    {
        ++reads_queued_;
        reads_.emplace_back(OptionalBytes{bytes::Bytes(b.begin(), b.end())});
    }
    void queue_no_frame()
    {
        ++reads_queued_;
        reads_.emplace_back(OptionalBytes{});
    }
    void queue_error(ErrorKind kind, std::string detail = {})
    {
        ++reads_queued_;
        reads_.emplace_back(fail(kind, std::move(detail)));
    }
    void queueBlockingRead()
    {
        std::lock_guard lock(mutex_);
        blocking_read_pending_ = true;
    }
    // Blocks until read() has actually entered the queued blocking read.
    // Tests use this instead of a wall-clock QTest::qWait() to reach the
    // "transport is mid-read" state before calling requestStop(), so what
    // they prove about unblocking does not depend on the worker thread
    // winning a race against a fixed sleep. Mirrors
    // ScriptedLoggingProtocol::waitUntilPollEntered.
    bool waitUntilBlockingReadEntered(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return blocking_read_entered_cv_.wait_for(lock, timeout, [this] { return blocking_read_entered_; });
    }
    bool scriptConsumed() const
    {
        return wIdx_ == expected_.size() && reads_.empty() && !blocking_read_pending_;
    }
    std::size_t writesConsumed() const
    {
        return wIdx_;
    }

    Status reset_connection() override
    {
        lifecycle_calls_.push_back("reset_connection");
        ++reset_call_count_;
        open_ = false;
        return reset_result_;
    }

    Status configure(const KlineConfig& config) override
    {
        lifecycle_calls_.push_back("configure");
        last_config_ = config;
        return configure_result_;
    }
    Status open() override
    {
        lifecycle_calls_.push_back("open");
        open_ = true;
        return open_result_;
    }
    Status close() override
    {
        lifecycle_calls_.push_back("close");
        ++close_call_count_;
        open_ = false;
        return close_result_;
    }
    Status disable_lec_lines() override
    {
        control_line_trace_.push_back(ControlLineAction::DisableLecLines);
        operation_trace_.push_back(Operation::DisableLecLines);
        return disable_lec_lines_result_;
    }
    Status pulse_lec_2_line(std::chrono::milliseconds timeout) override
    {
        control_line_trace_.push_back(ControlLineAction::PulseLec2);
        operation_trace_.push_back(Operation::PulseLec2);
        lec_2_pulse_timeouts_.push_back(timeout);
        return pulse_lec_2_line_result_;
    }
    Status enable_programming_voltage_line() override
    {
        programming_voltage_line_write_index_ = wIdx_;
        control_line_trace_.push_back(ControlLineAction::EnableProgrammingVoltageLine);
        operation_trace_.push_back(Operation::EnableProgrammingVoltageLine);
        return enable_programming_voltage_line_result_;
    }
    Status enable_boot_mode_lines() override
    {
        control_line_trace_.push_back(ControlLineAction::EnableBootModeLines);
        operation_trace_.push_back(Operation::EnableBootModeLines);
        return enable_boot_mode_lines_result_;
    }
    bool requires_post_kernel_upload_delay() const override
    {
        return post_kernel_upload_delay_required_;
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
    Status setBaud(int baud) override
    {
        baud_calls_.push_back(baud);
        return set_baud_result_;
    }
    Result<std::size_t> write(bytes::ByteView data) override
    {
        const bytes::Bytes actual(data.begin(), data.end());
        if (wIdx_ >= expected_.size())
        {
            return fail(ErrorKind::Internal,
                        std::format("scripted K-Line write ran past the end of the script ({} exchanges); wrote {}",
                                    expected_.size(), bytes::toHex(actual)));
        }
        if (expected_.at(wIdx_) != actual)
        {
            return fail(ErrorKind::Internal, describeDivergence(wIdx_, actual));
        }
        if (std::find(raw_writes_.begin(), raw_writes_.end(), wIdx_) != raw_writes_.end())
        {
            return fail(ErrorKind::Internal, "expected raw K-Line write");
        }
        ++wIdx_;
        return data.size();
    }
    Result<std::size_t> write_raw(bytes::ByteView data) override
    {
        const bytes::Bytes actual(data.begin(), data.end());
        if (wIdx_ >= expected_.size() || expected_[wIdx_] != actual ||
            std::find(raw_writes_.begin(), raw_writes_.end(), wIdx_) == raw_writes_.end())
        {
            return fail(ErrorKind::Internal, "unexpected raw K-Line write");
        }
        ++wIdx_;
        return data.size();
    }
    Result<OptionalBytes> read_raw(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (std::find(raw_reads_.begin(), raw_reads_.end(), reads_consumed_) == raw_reads_.end())
        {
            return fail(ErrorKind::Internal, "unexpected raw K-Line read");
        }
        return read_impl(timeout, cancellation);
    }
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (std::find(raw_reads_.begin(), raw_reads_.end(), reads_consumed_) != raw_reads_.end())
        {
            return fail(ErrorKind::Internal, "expected raw K-Line read");
        }
        return read_impl(timeout, cancellation);
    }
    Result<OptionalBytes> read_impl(std::chrono::milliseconds timeout, const ICancellationToken& cancellation)
    {
        read_timeouts_.push_back(timeout);
        if (timeout == std::chrono::milliseconds{10})
        {
            operation_trace_.push_back(Operation::Read10);
        }
        {
            std::unique_lock lock(mutex_);
            if (blocking_read_pending_)
            {
                blocking_read_entered_ = true;
                blocking_read_entered_cv_.notify_all();
                cv_.wait(lock, [this] { return unblock_requested_; });
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
        ++reads_consumed_;
        return result;
    }

    int close_call_count_ = 0;
    // Lifecycle calls in order, as ScriptedCanFlashTransport records them, so
    // a test can pin a reset before configure().
    std::vector<std::string> lifecycle_calls_;
    int reset_call_count_ = 0;
    Status reset_result_;
    Status configure_result_;
    Status open_result_;
    Status close_result_;
    Status disable_lec_lines_result_;
    Status pulse_lec_2_line_result_;
    Status enable_programming_voltage_line_result_;
    Status enable_boot_mode_lines_result_;
    bool post_kernel_upload_delay_required_ = false;
    std::optional<KlineConfig> last_config_;
    std::vector<ControlLineAction> control_line_trace_;
    std::vector<std::chrono::milliseconds> lec_2_pulse_timeouts_;
    std::vector<std::chrono::milliseconds> read_timeouts_;
    std::vector<Operation> operation_trace_;
    std::optional<std::size_t> programming_voltage_line_write_index_;

    // Records every set_add_iso14230_header() call in order (true == "add
    // header", false == "don't") so tests can assert the exact transitions
    // relative to connect/upload/read -- see denso_sh705x_eeprom_kline_
    // executor.cpp's execute() for the call sites this proves.
    std::vector<bool> header_mode_calls_;
    Status set_add_iso14230_header_result_;
    std::vector<int> baud_calls_;
    Status set_baud_result_;

  private:
    std::string describeDivergence(std::size_t index, const bytes::Bytes& actual) const
    {
        const std::string& label = sections_.at(index);
        const std::string where = label.empty()
                                      ? std::format("scripted K-Line exchange #{}", index + 1)
                                      : std::format("scripted K-Line exchange #{} (\"{}\")", index + 1, label);
        return std::format("{} diverged\n  expected: {}\n  actual:   {}", where, bytes::toHex(expected_.at(index)),
                           bytes::toHex(actual));
    }

    std::vector<std::size_t> raw_writes_;
    std::vector<std::size_t> raw_reads_;
    std::size_t reads_consumed_ = 0;
    std::size_t reads_queued_ = 0;
    std::vector<bytes::Bytes> expected_;
    std::vector<std::string> sections_;
    std::string current_section_;
    std::deque<Result<OptionalBytes>> reads_;
    std::size_t wIdx_ = 0;
    bool open_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable blocking_read_entered_cv_;
    bool blocking_read_pending_ = false;
    bool blocking_read_entered_ = false;
    bool unblock_requested_ = false;
};

} // namespace fastecu::flash
