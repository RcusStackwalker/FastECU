#pragma once
#include <functional>
#include <optional>
#include <vector>

#include "apps/bench/bench_driver.h"
#include "apps/bench/testing/fake_bench_session.h"

namespace fastecu::bench::testing
{

class FakeBenchEnvironment final : public IBenchEnvironment
{
  public:
    explicit FakeBenchEnvironment(FakeBenchSession& session) : session_(session)
    {
    }

    Result<std::vector<std::string>> list_ports(const GlobalOptions&) override
    {
        ++port_calls;
        if (port_error.has_value())
        {
            return std::unexpected(*port_error);
        }
        return ports;
    }

    Result<std::reference_wrapper<IBenchSession>> session(const GlobalOptions& options,
                                                          bool connect_implicitly) override
    {
        ++session_calls;
        implicit_connect_requests.push_back(connect_implicitly);
        session_options.push_back(options);
        if (session_error.has_value())
        {
            return std::unexpected(*session_error);
        }
        return std::ref(static_cast<IBenchSession&>(session_));
    }

    const TrafficEvidence& last_setup_traffic() const override
    {
        return setup_traffic;
    }

    std::vector<std::string> ports;
    std::optional<Error> port_error;
    std::optional<Error> session_error;
    TrafficEvidence setup_traffic;
    int port_calls = 0;
    int session_calls = 0;
    std::vector<bool> implicit_connect_requests;
    std::vector<GlobalOptions> session_options;

  private:
    FakeBenchSession& session_;
};

} // namespace fastecu::bench::testing
