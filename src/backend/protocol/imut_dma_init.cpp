#include "src/backend/protocol/imut_dma_init.h"
namespace mutdma
{
bool FiveBaudInit::wake(IKlineTransport& t)
{
    // Scaffold: sets the DMA link baud. The physical 5-baud slow-init pulse (and its
    // address byte addr_) is the one carried VERIFY item and is wired during bench
    // bring-up - at which point IKlineTransport gains a fiveBaudInit() method backed
    // by SerialPortActions::five_baud_init, called here before setBaud(baud_).
    return t.setBaud(baud_);
}
} // namespace mutdma
