#include "protocol/mut_dma_freeform.h"
#include "protocol/mut_dma_codec.h"
namespace mutdma {
QByteArray buildSetupFrame(quint8 setupCmd, quint8 channelCount) {
    QByteArray payload; payload.append(char(channelCount));
    return buildCommandFrame(setupCmd, payload, TRAILER_FREEFORM);
}
quint8 sizeToDescriptor(quint8 len) {
    switch (len) { case 1: return 0; case 2: return 1; case 4: return 2; default: return 0; }
}
int reqLen(int channelCount) {
    return ((channelCount + 3) >> 2) + channelCount * 2 + 0x1c;
}
}
