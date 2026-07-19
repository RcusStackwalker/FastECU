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
    int write(std::uint32_t canId, bytes::ByteView payload) override;
    bytes::Bytes read(int timeoutMs, std::uint32_t& outId) override;
    bool isOpen() const override;

  private:
    SerialPortActions *serial_;
};
} // namespace cdbg
