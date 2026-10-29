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
class ScriptedUdsChannel : public IUdsChannel
{
  public:
    // A matched expectation is consumed even when its scripted send fails.
    void ExpectSend(bytes::ByteView pdu, fastecu::Status outcome = {})
    {
        expected_.emplace_back(pdu.begin(), pdu.end());
        send_results_.push_back(std::move(outcome));
    }
    void QueueReceive(bytes::ByteView pdu)
    {
        QueueOutcome(bytes::Bytes(pdu.begin(), pdu.end()));
    }
    void QueueNoFrame()
    {
        QueueOutcome(std::nullopt);
    }
    void QueueError(fastecu::ErrorKind kind, std::string detail = {})
    {
        QueueOutcome(fastecu::Fail(kind, std::move(detail)));
    }
    void QueueOutcome(fastecu::Result<std::optional<bytes::Bytes>> outcome)
    {
        receives_.push_back(std::move(outcome));
    }

    // Includes cancelled and unexpected attempts, unlike SendsConsumed().
    std::size_t SendCalls() const
    {
        return send_calls_;
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
        ++send_calls_;
        if (cancellation.Cancelled())
        {
            return fastecu::Fail(fastecu::ErrorKind::kCancelled, "scripted UDS send cancelled");
        }
        if (send_index_ >= expected_.size() || expected_.at(send_index_) != bytes::Bytes(pdu.begin(), pdu.end()))
        {
            return fastecu::Fail(fastecu::ErrorKind::kInternal, "unexpected scripted UDS send");
        }
        return send_results_.at(send_index_++);
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
    std::vector<fastecu::Status> send_results_;
    std::size_t send_calls_ = 0;
    std::deque<fastecu::Result<std::optional<bytes::Bytes>>> receives_;
    std::size_t send_index_ = 0;
};

} // namespace uds
