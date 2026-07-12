#include "j2534_bridge_client.h"

#include <cassert>
#include <cstdio>

int main()
{
    J2534BridgeClient client("j2534_bridge_host.exe", "fake_j2534_dll.dll");
    assert(client.start() && "client failed to spawn the bridge host");
    assert(client.isRunning());

    unsigned long deviceId = 0;
    long result = client.PassThruOpen(nullptr, &deviceId);
    assert(result == STATUS_NOERROR);
    assert(deviceId == 7);

    unsigned long channelId = 0;
    result = client.PassThruConnect(deviceId, ISO9141, 0, 0, &channelId);
    assert(result == STATUS_NOERROR);
    assert(channelId == 3);

    PASSTHRU_MSG msg{};
    unsigned long numMsgs = 1;
    result = client.PassThruReadMsgs(channelId, &msg, &numMsgs, 100);
    assert(result == STATUS_NOERROR);
    assert(numMsgs == 1);
    assert(msg.DataSize == 4 && msg.Data[0] == 0xDE);

    std::printf("All j2534_bridge_client tests passed.\n");
    return 0;
}
