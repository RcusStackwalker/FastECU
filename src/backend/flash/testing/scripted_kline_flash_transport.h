#pragma once
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/scripted_flash_transport_state.h"

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
    };
    enum class Operation
    {
        DisableLecLines,
        PulseLec2,
        EnableProgrammingVoltageLine,
        Read10,
    };

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
    Status disable_lec_lines() override
    {
        control_line_trace_.push_back(ControlLineAction::DisableLecLines);
        operation_trace_.push_back(Operation::DisableLecLines);
        return disable_lec_lines_result_;
    }
    Status pulse_lec_2_line(int timeout_ms) override
    {
        control_line_trace_.push_back(ControlLineAction::PulseLec2);
        operation_trace_.push_back(Operation::PulseLec2);
        lec_2_pulse_timeouts_.push_back(timeout_ms);
        return pulse_lec_2_line_result_;
    }
    Status enable_programming_voltage_line() override
    {
        programming_voltage_line_write_index_ = wIdx_;
        control_line_trace_.push_back(ControlLineAction::EnableProgrammingVoltageLine);
        operation_trace_.push_back(Operation::EnableProgrammingVoltageLine);
        return enable_programming_voltage_line_result_;
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
        if (wIdx_ >= expected_.size() || expected_.at(wIdx_) != bytes::Bytes(data.begin(), data.end()))
        {
            return fail(ErrorKind::Internal, "unexpected scripted K-Line write");
        }
        ++wIdx_;
        return data.size();
    }
    Result<OptionalBytes> read(int timeout_ms, const ICancellationToken& cancellation) override
    {
        read_timeouts_.push_back(timeout_ms);
        if (timeout_ms == 10)
        {
            operation_trace_.push_back(Operation::Read10);
        }
        {
            std::unique_lock lock(mutex_);
            if (blocking_read_pending_)
            {
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
        return result;
    }

    int close_call_count_ = 0;
    Status configure_result_;
    Status open_result_;
    Status close_result_;
    Status disable_lec_lines_result_;
    Status pulse_lec_2_line_result_;
    Status enable_programming_voltage_line_result_;
    bool post_kernel_upload_delay_required_ = false;
    std::optional<KlineConfig> last_config_;
    std::vector<ControlLineAction> control_line_trace_;
    std::vector<int> lec_2_pulse_timeouts_;
    std::vector<int> read_timeouts_;
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
