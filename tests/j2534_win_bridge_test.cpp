#include "J2534_win.h"

#include <cassert>
#include <cstdio>

int main()
{
    J2534 j2534;
    j2534.setDllName("fake_j2534_dll.dll"); // built for x86; this test process is x64 (host arch)

    unsigned long deviceId = 0;
    long result = j2534.PassThruOpen(nullptr, &deviceId);
    assert(result == STATUS_NOERROR && "PassThruOpen should transparently succeed via the bridge");
    assert(deviceId == 7);

    unsigned long channelId = 0;
    result = j2534.PassThruConnect(deviceId, ISO9141, 0, 0, &channelId);
    assert(result == STATUS_NOERROR);
    assert(channelId == 3);

    PASSTHRU_MSG msg{};
    unsigned long numMsgs = 1;
    result = j2534.PassThruReadMsgs(channelId, &msg, &numMsgs, 100);
    assert(result == STATUS_NOERROR);
    assert(msg.DataSize == 4 && msg.Data[0] == 0xDE);

    std::printf("All j2534_win_bridge tests passed.\n");
    return 0;
}
