#pragma once

#include "src/backend/protocol/uds/iuds_channel.h"

#include <chrono>
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
    void ExpectSend(bytes::ByteView pdu)
    {
        expected_.emplace_back(pdu.begin(), pdu.end());
    }
    void QueueReceive(bytes::ByteView pdu)
    {
        receives_.emplace_back(std::optional<bytes::Bytes>{bytes::Bytes(pdu.begin(), pdu.end())});
    }
    void QueueNoFrame()
    {
        receives_.emplace_back(std::optional<bytes::Bytes>{});
    }
    void QueueError(fastecu::ErrorKind kind, std::string detail = {})
    {
        receives_.emplace_back(fastecu::Fail(kind, std::move(detail)));
    }

    std::size_t SendsConsumed() const
    {
        return send_index_;
    }
    bool ScriptConsumed() const
    {
        return send_index_ == expected_.size() && receives_.empty();
    }

    fastecu::Status Send(bytes::ByteView pdu, const fastecu::ICancellationToken& cancellation) override
    {
        if (cancellation.Cancelled())
        {
            return fastecu::Fail(fastecu::ErrorKind::kCancelled, "scripted UDS send cancelled");
        }
        if (send_index_ >= expected_.size() || expected_.at(send_index_) != bytes::Bytes(pdu.begin(), pdu.end()))
        {
            return fastecu::Fail(fastecu::ErrorKind::kInternal, "unexpected scripted UDS send");
        }
        ++send_index_;
        return {};
    }

    fastecu::Result<std::optional<bytes::Bytes>> Receive(std::chrono::milliseconds timeout,
                                                         const fastecu::ICancellationToken& cancellation) override
    {
        last_timeout = timeout;
        timeouts.push_back(timeout);
        if (cancellation.Cancelled())
        {
            return fastecu::Fail(fastecu::ErrorKind::kCancelled, "scripted UDS receive cancelled");
        }
        if (receives_.empty())
        {
            return fastecu::Fail(fastecu::ErrorKind::kInternal, "no scripted UDS receive outcome");
        }
        auto result = std::move(receives_.front());
        receives_.pop_front();
        return result;
    }

    std::chrono::milliseconds last_timeout{0};
    std::vector<std::chrono::milliseconds> timeouts;

  private:
    std::vector<bytes::Bytes> expected_;
    std::deque<fastecu::Result<std::optional<bytes::Bytes>>> receives_;
    std::size_t send_index_ = 0;
};

} // namespace uds
