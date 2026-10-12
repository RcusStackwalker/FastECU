#pragma once

#include <memory>

#include "src/backend/flash/flash_transport_source.h"

class SerialPortActions;

namespace fastecu::flash
{

// Desktop transports over one SerialPortActions, which must outlive every
// transport made here. A null serial is accepted, as by the transports
// themselves; it supplies no programming voltage, so the workflow prompts.
class DesktopFlashTransportSource final : public IFlashTransportSource
{
  public:
    explicit DesktopFlashTransportSource(SerialPortActions *serial);

    std::unique_ptr<IKlineFlashTransport> MakeKline() override;
    std::unique_ptr<ICanFlashTransport> MakeCan() override;
    std::unique_ptr<IMixedCanFlashTransport> MakeMixedCan() override;
    // OpenPort 2.0 drives programming voltage on the LEC line on every
    // platform. Asked on every call: the adapter setting may change.
    bool SuppliesProgrammingVoltage() const override;

  private:
    SerialPortActions *serial_;
};

} // namespace fastecu::flash
