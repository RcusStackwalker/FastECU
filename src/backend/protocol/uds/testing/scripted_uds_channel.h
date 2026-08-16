#pragma once

#include "src/backend/protocol/uds/iuds_channel.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uds
{

// Scripted IUdsChannel for exercising UdsClient without a transport.
// Modeled on fastecu::flash::ScriptedCanFlashTransport
// (src/backend/flash/testing/scripted_can_flash_transport.h): sends are
// matched against an expected sequence, receives are replayed from a queue.
//
// A send that does not match the next expectation fails with ErrorKind::
// Internal rather than an assertion, so a test sees the mismatch as a
// returned Error at the point of use.
class ScriptedUdsChannel final : public IUdsChannel
{
  public:
    void expectSend(bytes::ByteView pdu)
    {
        expected_.emplace_back(pdu.begin(), pdu.end());
    }
    void queueReceive(bytes::ByteView pdu)
    {
        receives_.emplace_back(std::optional<bytes::Bytes>{bytes::Bytes(pdu.begin(), pdu.end())});
    }
    void queueNoFrame()
    {
        receives_.emplace_back(std::optional<bytes::Bytes>{});
    }
    void queueError(fastecu::ErrorKind kind, std::string detail = {})
    {
        receives_.emplace_back(fastecu::fail(kind, std::move(detail)));
    }

    std::size_t sendsConsumed() const
    {
        return send_index_;
    }
    bool scriptConsumed() const
    {
        return send_index_ == expected_.size() && receives_.empty();
    }

    fastecu::Status send(bytes::ByteView pdu, const fastecu::ICancellationToken& cancellation) override
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted UDS send cancelled");
        }
        if (send_index_ >= expected_.size() || expected_.at(send_index_) != bytes::Bytes(pdu.begin(), pdu.end()))
        {
            return fastecu::fail(fastecu::ErrorKind::Internal, "unexpected scripted UDS send");
        }
        ++send_index_;
        return {};
    }

    fastecu::Result<std::optional<bytes::Bytes>> receive(int timeout_ms,
                                                         const fastecu::ICancellationToken& cancellation) override
    {
        last_timeout_ms_ = timeout_ms;
        timeouts_.push_back(timeout_ms);
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted UDS receive cancelled");
        }
        if (receives_.empty())
        {
            return fastecu::fail(fastecu::ErrorKind::Internal, "no scripted UDS receive outcome");
        }
        auto result = std::move(receives_.front());
        receives_.pop_front();
        return result;
    }

    int last_timeout_ms_ = 0;
    std::vector<int> timeouts_;

  private:
    std::vector<bytes::Bytes> expected_;
    std::deque<fastecu::Result<std::optional<bytes::Bytes>>> receives_;
    std::size_t send_index_ = 0;
};

} // namespace uds
