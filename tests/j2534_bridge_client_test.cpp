#include "src/platform/desktop/windows/j2534/j2534_bridge_client.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>

TEST(J2534BridgeClient, OpensConnectsAndReadsThroughBridge)
{
    // The host exe and fake DLL are 32-bit artifacts built outside Bazel
    // (scripts/compile-x86-bridge-artifacts.ps1 -- this Bazel setup has no
    // registered x86 Windows C++ toolchain), so CI passes their paths in via
    // J2534_BRIDGE_HOST_EXE/FAKE_J2534_DLL_PATH environment variables
    // (--test_env in .github/workflows/pr.yml). The bare filenames are kept
    // as a fallback for running this binary manually outside CI, with both
    // files copied next to it.
    const char *host_exe = std::getenv("J2534_BRIDGE_HOST_EXE");
    if (!host_exe)
    {
        host_exe = "j2534_bridge_host.exe";
    }
    const char *dll_path = std::getenv("FAKE_J2534_DLL_PATH");
    if (!dll_path)
    {
        dll_path = "fake_j2534_dll.dll";
    }

    J2534BridgeClient client(host_exe, dll_path);
    ASSERT_TRUE(client.start() && "client failed to spawn the bridge host");
    ASSERT_TRUE(client.isRunning());

    unsigned long device_id = 0;
    long result = client.PassThruOpen(nullptr, &device_id);
    ASSERT_TRUE(result == kJ2534StatusNoerror);
    ASSERT_TRUE(device_id == 7);

    unsigned long channel_id = 0;
    result = client.PassThruConnect(device_id, kJ2534Iso9141, 0, 0, &channel_id);
    ASSERT_TRUE(result == kJ2534StatusNoerror);
    ASSERT_TRUE(channel_id == 3);

    PassThruMsg msg{};
    unsigned long num_msgs = 1;
    result = client.PassThruReadMsgs(channel_id, &msg, &num_msgs, 100);
    ASSERT_TRUE(result == kJ2534StatusNoerror);
    ASSERT_TRUE(num_msgs == 1);
    ASSERT_TRUE(msg.data_size == 4 && msg.data[0] == 0xDE);

    std::printf("All j2534_bridge_client tests passed.\n");
}
