#include "src/platform/desktop/windows/j2534/J2534_win.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include <windows.h>

namespace
{

bool fileExists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// J2534_win.cpp's checkDLL() spawns the 32-bit bridge helper by the bare,
// hardcoded name "j2534_bridge_host.exe" relative to the test process's own
// working directory -- that's production behaviour we can't (and shouldn't)
// change here. So when running under Bazel, where the real x86-built
// j2534_bridge_host.exe lives in a separate --platforms=windows_x86 build
// output tree (see J2534_BRIDGE_HOST_EXE), stage a copy next to this test
// binary under that exact name before exercising any PassThru* call.
void ensureBridgeHostStaged()
{
    const char *hostSrc = std::getenv("J2534_BRIDGE_HOST_EXE");
    if (!hostSrc)
        return;
    if (fileExists("j2534_bridge_host.exe"))
        return;
    BOOL ok = CopyFileA(hostSrc, "j2534_bridge_host.exe", /*bFailIfExists=*/FALSE);
    assert(ok && "failed to stage j2534_bridge_host.exe next to the test binary");
    (void)ok;
}

} // namespace

int main()
{
    ensureBridgeHostStaged();

    const char *dllPath = std::getenv("FAKE_J2534_DLL_PATH");
    if (!dllPath)
        dllPath = "fake_j2534_dll.dll"; // built for x86; this test process is x64 (host arch)

    J2534 j2534;
    j2534.setDllName(dllPath);

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
