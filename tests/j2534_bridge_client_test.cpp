#include "src/platform/desktop/windows/j2534/j2534_bridge_client.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

int main()
{
    // The host exe and fake DLL are 32-bit artifacts built outside Bazel
    // (scripts/compile-x86-bridge-artifacts.ps1 -- this Bazel setup has no
    // registered x86 Windows C++ toolchain), so CI passes their paths in via
    // J2534_BRIDGE_HOST_EXE/FAKE_J2534_DLL_PATH environment variables
    // (--test_env in .github/workflows/pr.yml). The bare filenames are kept
    // as a fallback for running this binary manually outside CI, with both
    // files copied next to it.
    const char *hostExe = std::getenv("J2534_BRIDGE_HOST_EXE");
    if (!hostExe)
        hostExe = "j2534_bridge_host.exe";
    const char *dllPath = std::getenv("FAKE_J2534_DLL_PATH");
    if (!dllPath)
        dllPath = "fake_j2534_dll.dll";

    J2534BridgeClient client(hostExe, dllPath);
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
