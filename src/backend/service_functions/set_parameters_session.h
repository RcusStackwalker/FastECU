#pragma once

#include <string>

#include "src/backend/service_functions/service_function_session.h"
#include "src/backend/service_functions/tcu_parameter_table.h"

namespace fastecu::service_functions
{

// Portable equivalent of FlashTcuSubaruDensoSH705xCanOperation::
// tcu_setparam_subaru_ssm (legacy :135-517). The legacy writes twelve frames
// but only its first is well-formed, so it aborts on the second and leaves the
// TCU with one correction applied and eleven not, uncommitted. This session
// composes and frames each write from tcu_parameter_writes().
//
// Runs on K-Line, not CAN: legacy :141-152 switches the session to ISO14230 at
// 4800 baud before any parameter I/O.
class SetParametersSession final : public ServiceFunctionSession
{
  public:
    SetParametersSession(std::string protocol, TcuParameterValues values);

    Result<SsmTransportConfig> transport_setup() const override;
    ServiceFunctionStep resume(ISsmTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               IEventSink& events) override;
    void submit(GateResponse response) override;

  private:
    std::string protocol_;
    TcuParameterValues values_;
    bool misused_{false};
};

} // namespace fastecu::service_functions
