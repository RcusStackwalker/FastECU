#pragma once
#include "src/backend/protocol/ikline_transport.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>
namespace mutdma
{
// Test double: assert the exact sequence of writes, feed canned reads in order.
class ScriptedKlineTransport : public IKlineTransport
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
    fastecu::Status setBaud(int) override
    {
        return {};
    }
    fastecu::Result<std::size_t> write(bytes::ByteView data) override
    {
        if (wIdx_ >= expected_.size() || expected_.at(wIdx_) != bytes::Bytes(data.begin(), data.end()))
        {
            ok_ = false;
            return fastecu::fail(fastecu::ErrorKind::Internal, "unexpected scripted K-Line write");
        }
        else
        {
            ++wIdx_;
        }
        return data.size();
    }
    fastecu::Result<OptionalBytes> read(
        int, const fastecu::ICancellationToken& cancellation) override
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted K-Line read cancelled");
        }
        if (reads_.empty())
        {
            return fastecu::fail(fastecu::ErrorKind::Internal, "no scripted K-Line read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        return result;
    }

  private:
    std::vector<bytes::Bytes> expected_;
    std::deque<fastecu::Result<OptionalBytes>> reads_;
    std::size_t wIdx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
} // namespace mutdma
