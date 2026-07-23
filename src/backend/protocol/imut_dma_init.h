#pragma once
#include "src/backend/protocol/ikline_transport.h"
namespace mutdma
{
class IMutDmaInit
{
  public:
    virtual ~IMutDmaInit() = default;
    // Bring the ECU to the MUT-DMA listening state.
    virtual fastecu::Status wake(IKlineTransport& t) = 0;
};
// For ROMs that already boot into DMA mode (or patched ROMs): just set the link baud.
class AlreadyInMode : public IMutDmaInit
{
  public:
    explicit AlreadyInMode(int baud) : baud_(baud)
    {
    }
    fastecu::Status wake(IKlineTransport& t) override
    {
        return t.setBaud(baud_);
    }

  private:
    int baud_;
};
// 5-baud slow init then switch to DMA baud. addrByte is the (bench-confirmable)
// wake address; baud is the DMA link rate (125000 fast / 62500 slow).
class FiveBaudInit : public IMutDmaInit
{
  public:
    FiveBaudInit(bytes::Byte addrByte, int baud) : addr_(addrByte), baud_(baud)
    {
    }
    fastecu::Status wake(IKlineTransport& t) override; // see .cpp
  private:
    [[maybe_unused]] bytes::Byte addr_;
    int baud_;
};
} // namespace mutdma
