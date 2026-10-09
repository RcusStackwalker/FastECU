#include "src/platform/desktop/windows/j2534/J2534_win.h"

#include <gtest/gtest.h>
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
    {
        return;
    }
    if (fileExists("j2534_bridge_host.exe"))
    {
        return;
    }
    BOOL ok = CopyFileA(hostSrc, "j2534_bridge_host.exe", /*bFailIfExists=*/FALSE);
    ASSERT_TRUE(ok && "failed to stage j2534_bridge_host.exe next to the test binary");
    (void)ok;
}

} // namespace

TEST(J2534WinBridge, OpensConnectsAndReadsThroughBridge)
{
    ASSERT_NO_FATAL_FAILURE(ensureBridgeHostStaged());

    const char *dllPath = std::getenv("FAKE_J2534_DLL_PATH");
    if (!dllPath)
    {
        dllPath = "fake_j2534_dll.dll"; // built for x86; this test process is x64 (host arch)
    }

    J2534 j2534;
    j2534.setDllName(dllPath);

    unsigned long deviceId = 0;
    long result = j2534.PassThruOpen(nullptr, &deviceId);
    ASSERT_TRUE(result == kJ2534StatusNoerror && "PassThruOpen should transparently succeed via the bridge");
    ASSERT_TRUE(deviceId == 7);

    unsigned long channelId = 0;
    result = j2534.PassThruConnect(deviceId, kJ2534Iso9141, 0, 0, &channelId);
    ASSERT_TRUE(result == kJ2534StatusNoerror);
    ASSERT_TRUE(channelId == 3);

    PassThruMsg msg{};
    unsigned long numMsgs = 1;
    result = j2534.PassThruReadMsgs(channelId, &msg, &numMsgs, 100);
    ASSERT_TRUE(result == kJ2534StatusNoerror);
    ASSERT_TRUE(msg.data_size == 4 && msg.data[0] == 0xDE);

    std::printf("All j2534_win_bridge tests passed.\n");
}
