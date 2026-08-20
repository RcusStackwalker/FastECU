#pragma once
#include <cstddef>
#include <vector>

#include "apps/bench/bench_session_interface.h"

namespace fastecu::bench::testing
{

// Scripted session: replies are dequeued in order, and every request is
// recorded so a test can assert the exact PDU a command built.
class FakeBenchSession : public IBenchSession
{
  public:
    std::vector<bytes::Bytes> requests;
    std::vector<Result<bytes::Bytes>> replies;
    std::vector<uds::ExchangePolicy> policies;
    std::vector<int> raw_timeouts;
    std::vector<bytes::Bytes> received_on_error;
    std::vector<std::uint64_t> elapsed_ms;
    std::size_t next_reply = 0;
    Result<double> battery = 11.676;
    Status connect_result = {};
    TrafficEvidence connect_traffic;
    int connect_calls = 0;

    Status connect() override
    {
        ++connect_calls;
        last_traffic_ = connect_traffic;
        return connect_result;
    }

    Result<bytes::Bytes> exchange(bytes::ByteView pdu, const uds::ExchangePolicy& policy) override
    {
        policies.push_back(policy);
        return take(pdu);
    }

    Result<bytes::Bytes> exchange_raw(bytes::ByteView pdu, int timeout_ms) override
    {
        raw_timeouts.push_back(timeout_ms);
        return take(pdu);
    }

    Result<double> vbatt() override
    {
        return battery;
    }

    const TrafficEvidence& last_traffic() const override
    {
        return last_traffic_;
    }

  private:
    Result<bytes::Bytes> take(bytes::ByteView pdu)
    {
        requests.emplace_back(pdu.begin(), pdu.end());
        last_traffic_ = TrafficEvidence{.exchange_count = 1,
                                        .tx = requests.back(),
                                        .last_tx = requests.back(),
                                        .elapsed_ms = next_reply < elapsed_ms.size() ? elapsed_ms[next_reply] : 1};
        if (next_reply >= replies.size())
        {
            return fail(ErrorKind::Internal, "FakeBenchSession ran out of scripted replies");
        }
        Result<bytes::Bytes> result = replies[next_reply];
        if (result.has_value())
        {
            last_traffic_.rx = *result;
        }
        else if (next_reply < received_on_error.size())
        {
            last_traffic_.rx = received_on_error[next_reply];
        }
        last_traffic_.last_rx = last_traffic_.rx;
        ++next_reply;
        return result;
    }

    TrafficEvidence last_traffic_;
};

} // namespace fastecu::bench::testing
