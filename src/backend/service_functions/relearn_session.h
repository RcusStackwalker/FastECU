#pragma once

#include <string>

#include "src/backend/service_functions/service_function_session.h"

namespace fastecu::service_functions
{

// Portable equivalent of FlashTcuSubaruDensoSH705xCanOperation::
// tcu_relearn_subaru_ssm (legacy :632-790), which always reports failure: its
// last statement is an unconditional return STATUS_ERROR (:786).
//
// Three corrections, all named in the flash qualification matrix:
//   1. completion reports success;
//   2. the status poll accepts 0xE8, the positive response to the 0xA8 it
//      sends, instead of the unreachable 0xF8;
//   3. the poll frame is composed directly. Legacy :740-747 rewrites the
//      9-byte step-two buffer at indices 9-11, three bytes past its end, so
//      the second status address never reaches the wire.
//
// The poll's terminal condition is deliberately NOT invented: the bound stays
// at 200 iterations and the last frame is surfaced for the bench.
class RelearnSession final : public ServiceFunctionSession
{
  public:
    explicit RelearnSession(std::string protocol);

    Result<SsmTransportConfig> transport_setup() const override;
    ServiceFunctionStep resume(ISsmTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               IEventSink& events) override;
    void submit(GateResponse response) override;

  private:
    enum class Stage
    {
        AwaitStaticSetupGate,
        WriteSteps,
        Poll,
        Done,
    };

    std::string protocol_;
    Stage stage_{Stage::AwaitStaticSetupGate};
    bool gate_outstanding_{false};
    bool declined_{false};
};

} // namespace fastecu::service_functions
