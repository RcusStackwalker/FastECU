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
    std::size_t next_reply = 0;
    Result<double> battery = 11.676;
    Status connect_result = {};
    int connect_calls = 0;

    Status connect() override
    {
        ++connect_calls;
        return connect_result;
    }

    Result<bytes::Bytes> exchange(bytes::ByteView pdu, const uds::ExchangePolicy&) override
    {
        return take(pdu);
    }

    Result<bytes::Bytes> exchange_raw(bytes::ByteView pdu, int) override
    {
        return take(pdu);
    }

    Result<double> vbatt() override
    {
        return battery;
    }

  private:
    Result<bytes::Bytes> take(bytes::ByteView pdu)
    {
        requests.emplace_back(pdu.begin(), pdu.end());
        if (next_reply >= replies.size())
        {
            return fail(ErrorKind::Internal, "FakeBenchSession ran out of scripted replies");
        }
        return replies[next_reply++];
    }
};

} // namespace fastecu::bench::testing
