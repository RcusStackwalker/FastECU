#pragma once
#include "src/backend/protocol/issm_transport.h"

#include <chrono>
#include <deque>
#include <string>
#include <utility>
#include <vector>

// Test double: assert the exact sequence of writes, feed canned reads in order.
// Mirrors src/backend/protocol/testing/scripted_kline_transport.h's shape.
class ScriptedSsmTransport : public fastecu::ISsmTransport
{
  public:
    void ExpectWrite(bytes::ByteView b)
    {
        expected_.emplace_back(b.begin(), b.end());
    }
    void QueueRead(bytes::ByteView b)
    {
        reads_.emplace_back(OptionalBytes{bytes::Bytes(b.begin(), b.end())});
    }
    void ExpectReadTimeout(std::chrono::milliseconds timeout)
    {
        expected_read_timeouts_.push_back(timeout);
    }
    void QueueNoFrame()
    {
        reads_.emplace_back(OptionalBytes{});
    }
    void QueueError(fastecu::ErrorKind kind, std::string detail = {})
    {
        reads_.emplace_back(fastecu::Fail(kind, std::move(detail)));
    }
    void QueueWriteError(fastecu::ErrorKind kind, std::string detail = {})
    {
        write_errors_.emplace_back(fastecu::Fail(kind, std::move(detail)));
    }
    bool ScriptConsumed() const
    {
        return w_idx_ == expected_.size() && reads_.empty() && expected_read_timeouts_.empty();
    }
    bool Ok() const
    {
        return ok_;
    }
    void SetOpen(bool open)
    {
        open_ = open;
    }
    bool IsOpen() const override
    {
        return open_;
    }

    fastecu::Result<std::size_t> Write(bytes::ByteView data) override
    {
        if (w_idx_ >= expected_.size() || expected_.at(w_idx_) != bytes::Bytes(data.begin(), data.end()))
        {
            ok_ = false;
            return fastecu::Fail(fastecu::ErrorKind::kInternal, "unexpected scripted SSM write");
        }
        else
        {
            ++w_idx_;
        }
        if (!write_errors_.empty())
        {
            auto result = std::move(write_errors_.front());
            write_errors_.pop_front();
            return result;
        }
        return data.size();
    }

    fastecu::Result<OptionalBytes> Read(std::chrono::milliseconds timeout,
                                        const fastecu::ICancellationToken& cancellation) override
    {
        if (cancellation.Cancelled())
        {
            return fastecu::Fail(fastecu::ErrorKind::kCancelled, "scripted SSM read cancelled");
        }
        if (!expected_read_timeouts_.empty())
        {
            const auto expected_timeout = expected_read_timeouts_.front();
            expected_read_timeouts_.pop_front();
            if (timeout != expected_timeout)
            {
                ok_ = false;
                return fastecu::Fail(fastecu::ErrorKind::kInternal, "unexpected scripted SSM read timeout");
            }
        }
        if (reads_.empty())
        {
            return fastecu::Fail(fastecu::ErrorKind::kInternal, "no scripted SSM read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        return result;
    }

  private:
    std::vector<bytes::Bytes> expected_;
    std::deque<fastecu::Result<OptionalBytes>> reads_;
    std::deque<std::chrono::milliseconds> expected_read_timeouts_;
    std::deque<fastecu::Result<std::size_t>> write_errors_;
    std::size_t w_idx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
