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
        expected_ids_.push_back(id);
        expected_payloads_.emplace_back(payload.begin(), payload.end());
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
        return w_idx_ == expected_ids_.size() && reads_.empty();
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
        if (w_idx_ >= expected_ids_.size() || expected_ids_.at(w_idx_) != id ||
            expected_payloads_.at(w_idx_) != bytes::Bytes(payload.begin(), payload.end()))
        {
            ok_ = false;
            return fastecu::fail(fastecu::ErrorKind::kInternal, "unexpected scripted CAN write");
        }
        else
        {
            ++w_idx_;
        }
        return payload.size();
    }
    fastecu::Result<std::optional<CanFrame>> read(std::chrono::milliseconds,
                                                  const fastecu::ICancellationToken& cancellation) override
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::kCancelled, "scripted CAN read cancelled");
        }
        if (reads_.empty())
        {
            return fastecu::fail(fastecu::ErrorKind::kInternal, "no scripted CAN read outcome");
        }
        auto result = std::move(reads_.front());
        reads_.pop_front();
        return result;
    }

  private:
    std::vector<std::uint32_t> expected_ids_;
    std::vector<bytes::Bytes> expected_payloads_;
    std::deque<fastecu::Result<std::optional<CanFrame>>> reads_;
    std::size_t w_idx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
} // namespace cdbg
