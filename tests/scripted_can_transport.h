#pragma once
#include "src/backend/protocol/ican_transport.h"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>
namespace cdbg
{
// Test double: assert the exact sequence of (id,payload) writes, feed canned
// (id,payload) reads in order.
class ScriptedCanTransport : public ICanTransport
{
  public:
    void expectWrite(std::uint32_t id, bytes::ByteView payload)
    {
        expectedIds_.push_back(id);
        expectedPayloads_.emplace_back(payload.begin(), payload.end());
    }
    void queueRead(std::uint32_t id, bytes::ByteView payload)
    {
        reads_.emplace_back(std::optional<CanFrame>{CanFrame{id, bytes::Bytes(payload.begin(), payload.end())}});
    }
    void queue_no_frame()
    {
        reads_.emplace_back(std::optional<CanFrame>{});
    }
    void queue_error(fastecu::ErrorKind kind, std::string detail = {})
    {
        reads_.emplace_back(fastecu::fail(kind, std::move(detail)));
    }
    bool scriptConsumed() const
    {
        return wIdx_ == expectedIds_.size() && reads_.empty();
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
    fastecu::Result<std::size_t> write(std::uint32_t id, bytes::ByteView payload) override
    {
        if (wIdx_ >= expectedIds_.size() || expectedIds_.at(wIdx_) != id || expectedPayloads_.at(wIdx_) != bytes::Bytes(payload.begin(), payload.end()))
        {
            ok_ = false;
            return fastecu::fail(fastecu::ErrorKind::Internal, "unexpected scripted CAN write");
        }
        else
        {
            ++wIdx_;
        }
        return payload.size();
    }
    fastecu::Result<std::optional<CanFrame>> read(
        int, const fastecu::ICancellationToken& cancellation) override
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted CAN read cancelled");
        }
        if (reads_.empty())
        {
            return fastecu::fail(fastecu::ErrorKind::Internal, "no scripted CAN read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        return result;
    }

  private:
    std::vector<std::uint32_t> expectedIds_;
    std::vector<bytes::Bytes> expectedPayloads_;
    std::deque<fastecu::Result<std::optional<CanFrame>>> reads_;
    std::size_t wIdx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
} // namespace cdbg
