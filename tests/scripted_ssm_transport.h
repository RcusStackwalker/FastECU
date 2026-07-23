#pragma once
#include "src/backend/protocol/issm_transport.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

// Test double: assert the exact sequence of writes, feed canned reads in order.
// Mirrors tests/scripted_kline_transport.h's shape.
class ScriptedSsmTransport : public ISsmTransport
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
    void queue_error(fastecu::ErrorKind kind, std::string detail = {})
    {
        reads_.emplace_back(fastecu::fail(kind, std::move(detail)));
    }
    void queue_write_error(fastecu::ErrorKind kind, std::string detail = {})
    {
        write_errors_.emplace_back(fastecu::fail(kind, std::move(detail)));
    }
    bool scriptConsumed() const
    {
        return wIdx_ == expected_.size() && reads_.empty();
    }
    bool ok() const
    {
        return ok_;
    }
    void setOpen(bool open)
    {
        open_ = open;
    }
    bool isOpen() const override
    {
        return open_;
    }

    fastecu::Result<std::size_t> write(bytes::ByteView data) override
    {
        if (wIdx_ >= expected_.size() || expected_.at(wIdx_) != bytes::Bytes(data.begin(), data.end()))
        {
            ok_ = false;
            return fastecu::fail(fastecu::ErrorKind::Internal, "unexpected scripted SSM write");
        }
        else
        {
            ++wIdx_;
        }
        if (!write_errors_.empty())
        {
            auto result = std::move(write_errors_.front());
            write_errors_.pop_front();
            return result;
        }
        return data.size();
    }

    fastecu::Result<OptionalBytes> read(
        int, const fastecu::ICancellationToken& cancellation) override
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted SSM read cancelled");
        }
        if (reads_.empty())
        {
            return fastecu::fail(fastecu::ErrorKind::Internal, "no scripted SSM read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        return result;
    }

  private:
    std::vector<bytes::Bytes> expected_;
    std::deque<fastecu::Result<OptionalBytes>> reads_;
    std::deque<fastecu::Result<std::size_t>> write_errors_;
    int wIdx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
