#pragma once

#include <string>

#include "src/backend/service_functions/service_function_session.h"

namespace fastecu::service_functions
{

// Portable equivalent of FlashTcuSubaruDensoSH705xCanOperation::
// tcu_readparam_subaru_ssm (legacy :517-632), which cannot succeed: its retry
// loop accepts only 0xF8 while the post-loop check demands 0xE8. This session
// accepts 0xE8, the correct positive response to the 0xA8 request it sends,
// and requires the 15 bytes the decode actually indexes.
//
// Has no operator gates: submit() is a contract violation and makes the next
// resume() fail with ErrorKind::Internal.
class ReadParametersSession final : public ServiceFunctionSession
{
  public:
    explicit ReadParametersSession(std::string protocol);

    Result<SsmTransportConfig> transport_setup() const override;
    ServiceFunctionStep resume(ISsmTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               IEventSink& events) override;
    void submit(GateResponse response) override;

  private:
    std::string protocol_;
    bool misused_{false};
};

} // namespace fastecu::service_functions
