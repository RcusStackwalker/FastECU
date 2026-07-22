#pragma once
#include "src/backend/protocol/ican_transport.h"
class SerialPortActions;
namespace cdbg
{
// Adapts FastECU's SerialPortActions raw-CAN mode to ICanTransport. Matches
// the wire convention already used by
// src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_densocan.cpp: each frame is 4
// big-endian CAN-id bytes followed by the payload.
class FastEcuCanTransport : public ICanTransport
{
  public:
    explicit FastEcuCanTransport(SerialPortActions *serial) : serial_(serial)
    {
    }
    fastecu::Result<std::size_t> write(
        std::uint32_t canId, bytes::ByteView payload) override;
    fastecu::Result<std::optional<CanFrame>> read(
        int timeoutMs, const fastecu::ICancellationToken& cancellation) override;
    bool isOpen() const override;

  private:
    SerialPortActions *serial_;
};
} // namespace cdbg
