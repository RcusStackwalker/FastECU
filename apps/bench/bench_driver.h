#pragma once
#include <functional>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "apps/bench/bench_args.h"
#include "apps/bench/bench_session_interface.h"

namespace fastecu::bench
{

// Hardware/runtime boundary for the pure CLI driver. Production implements
// this with the desktop transport; tests use a package-owned fake that never
// loads or opens a J2534 adapter.
class IBenchEnvironment
{
  public:
    virtual ~IBenchEnvironment() = default;
    virtual Result<std::vector<std::string>> ListPorts(const GlobalOptions& options) = 0;
    virtual Result<std::reference_wrapper<IBenchSession>> Session(const GlobalOptions& options,
                                                                  bool connect_implicitly) = 0;
    virtual const TrafficEvidence& LastSetupTraffic() const = 0;
};

// Parses and executes one CLI invocation. JSON output is newline-delimited on
// stdout; human diagnostics are always duplicated to stderr.
int RunCli(IBenchEnvironment& environment, IBenchFiles& files, std::span<const std::string_view> args,
           std::istream& input, std::ostream& output, std::ostream& diagnostics);

} // namespace fastecu::bench
