#pragma once
#include "src/backend/protocol/idiagnostic_link.h"

#include <deque>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace fastecu::diagnostics
{

// Test double: records every call as a readable line in `calls`, and serves
// queued outcomes. An empty read queue answers "no frame"; empty open,
// five-baud and fast-init queues answer success / an empty response.
class FakeDiagnosticLink final : public IDiagnosticLink
{
  public:
    std::vector<std::string> calls;
    bool j2534 = false;

    void queue_open(Status outcome)
    {
        opens_.push_back(std::move(outcome));
    }
    void queue_five_baud(bytes::Bytes response)
    {
        five_bauds_.push_back(std::move(response));
    }
    void queue_fast_init(Status outcome)
    {
        fast_inits_.push_back(std::move(outcome));
    }
    void queue_read(bytes::Bytes frame)
    {
        reads_.emplace_back(OptionalBytes{std::move(frame)});
    }
    void queue_no_frame()
    {
        reads_.emplace_back(OptionalBytes{});
    }
    void queue_read_error(ErrorKind kind)
    {
        reads_.emplace_back(fail(kind, "scripted read error"));
    }
    bool script_consumed() const
    {
        return opens_.empty() && five_bauds_.empty() && fast_inits_.empty() && reads_.empty();
    }

    Status open(const KlineLinkConfig& c) override
    {
        calls.push_back(std::format("open kline header={} iso14230={} baud={} start={:02X} tester={:02X} target={:02X}",
                                    to_string(c.header), c.iso14230_connection, c.baud, c.start_byte, c.tester_id,
                                    c.target_id));
        return next(opens_);
    }
    Status open(const CanLinkConfig& c) override
    {
        calls.push_back(std::format("open can iso15765={} bitrate={} extended={} source={:03X} destination={:03X}",
                                    c.iso15765, c.bitrate, c.extended_id, c.source_id, c.destination_id));
        return next(opens_);
    }
    Status reset() override
    {
        calls.emplace_back("reset");
        return {};
    }
    Status set_header(KlineHeader header) override
    {
        calls.push_back(std::format("set_header {}", to_string(header)));
        return {};
    }
    Status set_p1_max(std::chrono::milliseconds p1_max) override
    {
        calls.push_back(std::format("p1 {}", p1_max.count()));
        return {};
    }
    Result<bytes::Bytes> five_baud_init(std::uint8_t address) override
    {
        calls.push_back(std::format("five_baud {:02X}", address));
        if (five_bauds_.empty())
        {
            return bytes::Bytes{};
        }
        auto response = std::move(five_bauds_.front());
        five_bauds_.pop_front();
        return response;
    }
    Status fast_init(bytes::ByteView wakeup) override
    {
        calls.push_back("fast_init " + hex(wakeup));
        return next(fast_inits_);
    }
    // Echoes the input unconditionally. The real adapter instead returns the
    // facade's echo-check result (which can legitimately differ from what
    // was written), so callers must not rely on this return value to assert
    // anything beyond "write was called" -- assert on `calls` instead.
    Result<bytes::Bytes> write(bytes::ByteView data) override
    {
        calls.push_back("write " + hex(data));
        return bytes::Bytes(data.begin(), data.end());
    }
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        calls.push_back(std::format("read {}", timeout.count()));
        return next_read(cancellation);
    }
    Result<OptionalBytes> read_obd(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        calls.push_back(std::format("read_obd {}", timeout.count()));
        return next_read(cancellation);
    }
    bool uses_j2534() const override
    {
        return j2534;
    }

    static std::string hex(bytes::ByteView data)
    {
        std::string out;
        for (const bytes::Byte b : data)
        {
            out += std::format("{}{:02X}", out.empty() ? "" : " ", b);
        }
        return out;
    }

  private:
    static Status next(std::deque<Status>& queue)
    {
        if (queue.empty())
        {
            return {};
        }
        auto outcome = std::move(queue.front());
        queue.pop_front();
        return outcome;
    }
    Result<OptionalBytes> next_read(const ICancellationToken& cancellation)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "scripted read cancelled");
        }
        if (reads_.empty())
        {
            return OptionalBytes{};
        }
        auto outcome = std::move(reads_.front());
        reads_.pop_front();
        return outcome;
    }

    std::deque<Status> opens_;
    std::deque<bytes::Bytes> five_bauds_;
    std::deque<Status> fast_inits_;
    std::deque<Result<OptionalBytes>> reads_;
};

} // namespace fastecu::diagnostics
