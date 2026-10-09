#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

using namespace j2534_bridge;

namespace
{

struct BridgeProcess
{
    HANDLE to_child_write = nullptr;
    HANDLE from_child_read = nullptr;
    PROCESS_INFORMATION pi{};

    bool start(const std::string& hostExePath, const std::string& dllPath)
    {
        HANDLE childStdinRead, childStdinWrite, childStdoutRead, childStdoutWrite;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

        if (!CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0))
        {
            return false;
        }
        if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0))
        {
            CloseHandle(childStdinRead);
            CloseHandle(childStdinWrite);
            return false;
        }
        SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = childStdinRead;
        si.hStdOutput = childStdoutWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        std::string cmdLine = "\"" + hostExePath + "\" \"" + dllPath + "\"";
        std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
        cmdLineBuf.push_back('\0');

        BOOL ok = CreateProcessA(nullptr, cmdLineBuf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(childStdinRead);
        CloseHandle(childStdoutWrite);
        if (!ok)
        {
            CloseHandle(childStdinWrite);
            CloseHandle(childStdoutRead);
            return false;
        }

        to_child_write = childStdinWrite;
        from_child_read = childStdoutRead;
        return true;
    }

    ~BridgeProcess()
    {
        stop();
    }

    void stop()
    {
        if (!pi.hProcess)
        {
            return;
        }
        writeFrame(to_child_write, Function::kShutdown, nullptr, 0);
        if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT)
        {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
        }
        CloseHandle(to_child_write);
        CloseHandle(from_child_read);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        pi = {};
        to_child_write = nullptr;
        from_child_read = nullptr;
    }
};

void test_open_connect_and_read(BridgeProcess& bridge)
{
    PassThruOpenRequest openReq{};
    openReq.has_name = false;
    writeFrame(bridge.to_child_write, Function::kPassThruOpen, &openReq, sizeof(openReq));
    FrameHeader header{};
    PassThruOpenResponse openResp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &openResp, sizeof(openResp)));
    ASSERT_TRUE(openResp.result == kJ2534StatusNoerror);
    ASSERT_TRUE(openResp.device_id == 7);

    PassThruConnectRequest connectReq{};
    connectReq.device_id = openResp.device_id;
    connectReq.protocol_id = kJ2534Iso9141;
    writeFrame(bridge.to_child_write, Function::kPassThruConnect, &connectReq, sizeof(connectReq));
    PassThruConnectResponse connectResp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &connectResp, sizeof(connectResp)));
    ASSERT_TRUE(connectResp.result == kJ2534StatusNoerror);
    ASSERT_TRUE(connectResp.channel_id == 3);

    PassThruReadMsgsRequest readReq{};
    readReq.channel_id = connectResp.channel_id;
    readReq.timeout = 100;
    writeFrame(bridge.to_child_write, Function::kPassThruReadMsgs, &readReq, sizeof(readReq));
    PassThruReadMsgsResponse readResp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &readResp, sizeof(readResp)));
    ASSERT_TRUE(readResp.result == kJ2534StatusNoerror);
    ASSERT_TRUE(readResp.num_msgs == 1);
    ASSERT_TRUE(readResp.msg.data_size == 4);
    ASSERT_TRUE(readResp.msg.data[0] == 0xDE && readResp.msg.data[1] == 0xAD && readResp.msg.data[2] == 0xBE &&
                readResp.msg.data[3] == 0xEF);

    std::printf("test_open_connect_and_read: PASS\n");
}

void test_write_msgs_success_and_failure(BridgeProcess& bridge)
{
    PassThruWriteMsgsRequest goodReq{};
    goodReq.channel_id = 3;
    goodReq.msg.data_size = 1;
    goodReq.msg.data[0] = 0x11;
    writeFrame(bridge.to_child_write, Function::kPassThruWriteMsgs, &goodReq, sizeof(goodReq));
    FrameHeader header{};
    PassThruWriteMsgsResponse goodResp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &goodResp, sizeof(goodResp)));
    ASSERT_TRUE(goodResp.result == kJ2534StatusNoerror);

    PassThruWriteMsgsRequest badReq = goodReq;
    badReq.msg.data[0] = 0x99;
    writeFrame(bridge.to_child_write, Function::kPassThruWriteMsgs, &badReq, sizeof(badReq));
    PassThruWriteMsgsResponse badResp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &badResp, sizeof(badResp)));
    ASSERT_TRUE(badResp.result == kJ2534ErrFailed);

    std::printf("test_write_msgs_success_and_failure: PASS\n");
}

void test_ioctl_read_vbatt(BridgeProcess& bridge)
{
    PassThruIoctlRequest req{};
    req.channel_id = 3;
    req.ioctl_id = kJ2534ReadVbatt;
    writeFrame(bridge.to_child_write, Function::kPassThruIoctl, &req, sizeof(req));
    FrameHeader header{};
    PassThruIoctlResponse resp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &resp, sizeof(resp)));
    ASSERT_TRUE(resp.result == kJ2534StatusNoerror);
    ASSERT_TRUE(resp.vbatt == 12500);

    std::printf("test_ioctl_read_vbatt: PASS\n");
}

void test_child_crash_is_detected_as_broken_pipe(const std::string& hostExe, const std::string& dllPath)
{
    BridgeProcess bridge;
    ASSERT_TRUE(bridge.start(hostExe, dllPath));

    TerminateProcess(bridge.pi.hProcess, 1);
    WaitForSingleObject(bridge.pi.hProcess, 2000);

    PassThruCloseRequest req{};
    req.device_id = 7;
    writeFrame(bridge.to_child_write, Function::kPassThruClose, &req, sizeof(req));
    FrameHeader header{};
    PassThruCloseResponse resp{};
    bool ok = readFrame(bridge.from_child_read, header, &resp, sizeof(resp));
    ASSERT_TRUE(!ok && "readFrame should report failure once the child process is dead");

    bridge.stop();

    std::printf("test_child_crash_is_detected_as_broken_pipe: PASS\n");
}

} // namespace

// The host exe and fake DLL paths are 32-bit artifacts built outside Bazel
// (scripts/compile-x86-bridge-artifacts.ps1 -- this Bazel setup has no
// registered x86 Windows C++ toolchain), so they are passed in via the
// J2534_BRIDGE_HOST_EXE/FAKE_J2534_DLL_PATH environment variables
// (--test_env in .github/workflows/pr.yml).
class J2534BridgeIntegration : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        const char *hostExeEnv = std::getenv("J2534_BRIDGE_HOST_EXE");
        const char *dllPathEnv = std::getenv("FAKE_J2534_DLL_PATH");
        host_exe_ = hostExeEnv ? hostExeEnv : "";
        dll_path_ = dllPathEnv ? dllPathEnv : "";
    }

    static std::string host_exe_;
    static std::string dll_path_;
};

std::string J2534BridgeIntegration::host_exe_;
std::string J2534BridgeIntegration::dll_path_;

TEST_F(J2534BridgeIntegration, CallsAndChildCrashContracts)
{
    ASSERT_FALSE(host_exe_.empty()) << "set J2534_BRIDGE_HOST_EXE";
    ASSERT_FALSE(dll_path_.empty()) << "set FAKE_J2534_DLL_PATH";

    BridgeProcess bridge;
    ASSERT_TRUE(bridge.start(host_exe_, dll_path_)) << "failed to spawn j2534_bridge_host";

    ASSERT_NO_FATAL_FAILURE(test_open_connect_and_read(bridge));
    ASSERT_NO_FATAL_FAILURE(test_write_msgs_success_and_failure(bridge));
    ASSERT_NO_FATAL_FAILURE(test_ioctl_read_vbatt(bridge));
    bridge.stop();

    ASSERT_NO_FATAL_FAILURE(test_child_crash_is_detected_as_broken_pipe(host_exe_, dll_path_));
}
