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
    void ExpectWrite(bytes::ByteView b)
    {
        expected_.emplace_back(b.begin(), b.end());
    }
    void QueueRead(bytes::ByteView b)
    {
        reads_.emplace_back(OptionalBytes{bytes::Bytes(b.begin(), b.end())});
    }
    void QueueNoFrame()
    {
        reads_.emplace_back(OptionalBytes{});
    }
    void QueueError(fastecu::ErrorKind kind, std::string detail = {})
    {
        reads_.emplace_back(fastecu::Fail(kind, std::move(detail)));
    }
    void QueueSetBaudError(fastecu::ErrorKind kind, std::string detail = {})
    {
        set_baud_results_.emplace_back(fastecu::Fail(kind, std::move(detail)));
    }
    void QueueWriteError(fastecu::ErrorKind kind, std::string detail = {})
    {
        write_results_.emplace_back(fastecu::Fail(kind, std::move(detail)));
    }
    bool ScriptConsumed() const
    {
        return w_idx_ == expected_.size() && reads_.empty() && set_baud_results_.empty() && write_results_.empty();
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
    fastecu::Status SetBaud(int) override
    {
        if (!set_baud_results_.empty())
        {
            auto result = std::move(set_baud_results_.front());
            set_baud_results_.pop_front();
            return result;
        }
        return {};
    }
    fastecu::Result<std::size_t> Write(bytes::ByteView data) override
    {
        if (w_idx_ >= expected_.size() || expected_.at(w_idx_) != bytes::Bytes(data.begin(), data.end()))
        {
            ok_ = false;
            return fastecu::Fail(fastecu::ErrorKind::kInternal, "unexpected scripted K-Line write");
        }
        else
        {
            ++w_idx_;
        }
        if (!write_results_.empty())
        {
            auto result = std::move(write_results_.front());
            write_results_.pop_front();
            return result;
        }
        return data.size();
    }
    fastecu::Result<OptionalBytes> Read(std::chrono::milliseconds,
                                        const fastecu::ICancellationToken& cancellation) override
    {
        if (cancellation.Cancelled())
        {
            return fastecu::Fail(fastecu::ErrorKind::kCancelled, "scripted K-Line read cancelled");
        }
        if (reads_.empty())
        {
            return fastecu::Fail(fastecu::ErrorKind::kInternal, "no scripted K-Line read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        return result;
    }

  private:
    std::vector<bytes::Bytes> expected_;
    std::deque<fastecu::Status> set_baud_results_;
    std::deque<fastecu::Result<std::size_t>> write_results_;
    std::deque<fastecu::Result<OptionalBytes>> reads_;
    std::size_t w_idx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
} // namespace mutdma
