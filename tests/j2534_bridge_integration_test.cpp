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

    bool start(const std::string& host_exe_path, const std::string& dll_path)
    {
        HANDLE child_stdin_read, child_stdin_write, child_stdout_read, child_stdout_write;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

        if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0))
        {
            return false;
        }
        if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0))
        {
            CloseHandle(child_stdin_read);
            CloseHandle(child_stdin_write);
            return false;
        }
        SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = child_stdin_read;
        si.hStdOutput = child_stdout_write;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        std::string cmd_line = "\"" + host_exe_path + "\" \"" + dll_path + "\"";
        std::vector<char> cmd_line_buf(cmd_line.begin(), cmd_line.end());
        cmd_line_buf.push_back('\0');

        BOOL ok = CreateProcessA(nullptr, cmd_line_buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdout_write);
        if (!ok)
        {
            CloseHandle(child_stdin_write);
            CloseHandle(child_stdout_read);
            return false;
        }

        to_child_write = child_stdin_write;
        from_child_read = child_stdout_read;
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
    PassThruOpenRequest open_req{};
    open_req.has_name = false;
    writeFrame(bridge.to_child_write, Function::kPassThruOpen, &open_req, sizeof(open_req));
    FrameHeader header{};
    PassThruOpenResponse open_resp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &open_resp, sizeof(open_resp)));
    ASSERT_TRUE(open_resp.result == kJ2534StatusNoerror);
    ASSERT_TRUE(open_resp.device_id == 7);

    PassThruConnectRequest connect_req{};
    connect_req.device_id = open_resp.device_id;
    connect_req.protocol_id = kJ2534Iso9141;
    writeFrame(bridge.to_child_write, Function::kPassThruConnect, &connect_req, sizeof(connect_req));
    PassThruConnectResponse connect_resp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &connect_resp, sizeof(connect_resp)));
    ASSERT_TRUE(connect_resp.result == kJ2534StatusNoerror);
    ASSERT_TRUE(connect_resp.channel_id == 3);

    PassThruReadMsgsRequest read_req{};
    read_req.channel_id = connect_resp.channel_id;
    read_req.timeout = 100;
    writeFrame(bridge.to_child_write, Function::kPassThruReadMsgs, &read_req, sizeof(read_req));
    PassThruReadMsgsResponse read_resp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &read_resp, sizeof(read_resp)));
    ASSERT_TRUE(read_resp.result == kJ2534StatusNoerror);
    ASSERT_TRUE(read_resp.num_msgs == 1);
    ASSERT_TRUE(read_resp.msg.data_size == 4);
    ASSERT_TRUE(read_resp.msg.data[0] == 0xDE && read_resp.msg.data[1] == 0xAD && read_resp.msg.data[2] == 0xBE &&
                read_resp.msg.data[3] == 0xEF);

    std::printf("test_open_connect_and_read: PASS\n");
}

void test_write_msgs_success_and_failure(BridgeProcess& bridge)
{
    PassThruWriteMsgsRequest good_req{};
    good_req.channel_id = 3;
    good_req.msg.data_size = 1;
    good_req.msg.data[0] = 0x11;
    writeFrame(bridge.to_child_write, Function::kPassThruWriteMsgs, &good_req, sizeof(good_req));
    FrameHeader header{};
    PassThruWriteMsgsResponse good_resp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &good_resp, sizeof(good_resp)));
    ASSERT_TRUE(good_resp.result == kJ2534StatusNoerror);

    PassThruWriteMsgsRequest bad_req = good_req;
    bad_req.msg.data[0] = 0x99;
    writeFrame(bridge.to_child_write, Function::kPassThruWriteMsgs, &bad_req, sizeof(bad_req));
    PassThruWriteMsgsResponse bad_resp{};
    ASSERT_TRUE(readFrame(bridge.from_child_read, header, &bad_resp, sizeof(bad_resp)));
    ASSERT_TRUE(bad_resp.result == kJ2534ErrFailed);

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

void test_child_crash_is_detected_as_broken_pipe(const std::string& host_exe, const std::string& dll_path)
{
    BridgeProcess bridge;
    ASSERT_TRUE(bridge.start(host_exe, dll_path));

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
        const char *host_exe_env = std::getenv("J2534_BRIDGE_HOST_EXE");
        const char *dll_path_env = std::getenv("FAKE_J2534_DLL_PATH");
        host_exe_ = host_exe_env ? host_exe_env : "";
        dll_path_ = dll_path_env ? dll_path_env : "";
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
