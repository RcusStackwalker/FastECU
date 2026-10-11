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

    void QueueOpen(Status outcome)
    {
        opens_.push_back(std::move(outcome));
    }
    void QueueFiveBaud(bytes::Bytes response)
    {
        five_bauds_.push_back(std::move(response));
    }
    void QueueFastInit(Status outcome)
    {
        fast_inits_.push_back(std::move(outcome));
    }
    void QueueRead(bytes::Bytes frame)
    {
        reads_.emplace_back(OptionalBytes{std::move(frame)});
    }
    void QueueNoFrame()
    {
        reads_.emplace_back(OptionalBytes{});
    }
    void QueueReadError(ErrorKind kind)
    {
        reads_.emplace_back(Fail(kind, "scripted read error"));
    }
    bool ScriptConsumed() const
    {
        return opens_.empty() && five_bauds_.empty() && fast_inits_.empty() && reads_.empty();
    }

    Status Open(const KlineLinkConfig& c) override
    {
        calls.push_back(
            std::format("open kline header={} iso14230={} baud={} start={:02X} tester={:02X} target={:02X}{}",
                        ToString(c.header), c.iso14230_connection, c.baud, c.start_byte, c.tester_id, c.target_id,
                        c.parity == Parity::kEven ? " parity=Even" : ""));
        return Next(opens_);
    }
    Status Open(const CanLinkConfig& c) override
    {
        calls.push_back(std::format("open can iso15765={} bitrate={} extended={} source={:03X} destination={:03X}",
                                    c.iso15765, c.bitrate, c.extended_id, c.source_id, c.destination_id));
        return Next(opens_);
    }
    Status Reset() override
    {
        calls.emplace_back("reset");
        return {};
    }
    Status SetHeader(KlineHeader header) override
    {
        calls.push_back(std::format("set_header {}", ToString(header)));
        return {};
    }
    Status SetP1Max(std::chrono::milliseconds p1_max) override
    {
        calls.push_back(std::format("p1 {}", p1_max.count()));
        return {};
    }
    Result<bytes::Bytes> FiveBaudInit(std::uint8_t address) override
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
    Status FastInit(bytes::ByteView wakeup) override
    {
        calls.push_back("fast_init " + Hex(wakeup));
        return Next(fast_inits_);
    }
    // Echoes the input unconditionally. The real adapter instead returns the
    // facade's echo-check result (which can legitimately differ from what
    // was written), so callers must not rely on this return value to assert
    // anything beyond "write was called" -- assert on `calls` instead.
    Result<bytes::Bytes> Write(bytes::ByteView data) override
    {
        calls.push_back("write " + Hex(data));
        return bytes::Bytes(data.begin(), data.end());
    }
    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        calls.push_back(std::format("read {}", timeout.count()));
        return NextRead(cancellation);
    }
    Result<OptionalBytes> ReadObd(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        calls.push_back(std::format("read_obd {}", timeout.count()));
        return NextRead(cancellation);
    }
    bool UsesJ2534() const override
    {
        return j2534;
    }

    static std::string Hex(bytes::ByteView data)
    {
        std::string out;
        for (const bytes::Byte b : data)
        {
            out += std::format("{}{:02X}", out.empty() ? "" : " ", b);
        }
        return out;
    }

  private:
    static Status Next(std::deque<Status>& queue)
    {
        if (queue.empty())
        {
            return {};
        }
        auto outcome = std::move(queue.front());
        queue.pop_front();
        return outcome;
    }
    Result<OptionalBytes> NextRead(const ICancellationToken& cancellation)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "scripted read cancelled");
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
