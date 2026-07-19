#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

#include <cassert>
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
    HANDLE toChildWrite = nullptr;
    HANDLE fromChildRead = nullptr;
    PROCESS_INFORMATION pi{};

    bool start(const std::string& hostExePath, const std::string& dllPath)
    {
        HANDLE childStdinRead, childStdinWrite, childStdoutRead, childStdoutWrite;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

        if (!CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0))
            return false;
        if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0))
            return false;
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
            return false;

        toChildWrite = childStdinWrite;
        fromChildRead = childStdoutRead;
        return true;
    }

    void stop()
    {
        writeFrame(toChildWrite, Function::Shutdown, nullptr, 0);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(toChildWrite);
        CloseHandle(fromChildRead);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
};

void test_open_connect_and_read(BridgeProcess& bridge)
{
    PassThruOpenRequest openReq{};
    openReq.hasName = false;
    writeFrame(bridge.toChildWrite, Function::PassThruOpen, &openReq, sizeof(openReq));
    FrameHeader header{};
    PassThruOpenResponse openResp{};
    assert(readFrame(bridge.fromChildRead, header, &openResp, sizeof(openResp)));
    assert(openResp.result == STATUS_NOERROR);
    assert(openResp.deviceId == 7);

    PassThruConnectRequest connectReq{};
    connectReq.deviceId = openResp.deviceId;
    connectReq.protocolId = ISO9141;
    writeFrame(bridge.toChildWrite, Function::PassThruConnect, &connectReq, sizeof(connectReq));
    PassThruConnectResponse connectResp{};
    assert(readFrame(bridge.fromChildRead, header, &connectResp, sizeof(connectResp)));
    assert(connectResp.result == STATUS_NOERROR);
    assert(connectResp.channelId == 3);

    PassThruReadMsgsRequest readReq{};
    readReq.channelId = connectResp.channelId;
    readReq.timeout = 100;
    writeFrame(bridge.toChildWrite, Function::PassThruReadMsgs, &readReq, sizeof(readReq));
    PassThruReadMsgsResponse readResp{};
    assert(readFrame(bridge.fromChildRead, header, &readResp, sizeof(readResp)));
    assert(readResp.result == STATUS_NOERROR);
    assert(readResp.numMsgs == 1);
    assert(readResp.msg.DataSize == 4);
    assert(readResp.msg.Data[0] == 0xDE && readResp.msg.Data[1] == 0xAD &&
           readResp.msg.Data[2] == 0xBE && readResp.msg.Data[3] == 0xEF);

    std::printf("test_open_connect_and_read: PASS\n");
}

void test_write_msgs_success_and_failure(BridgeProcess& bridge)
{
    PassThruWriteMsgsRequest goodReq{};
    goodReq.channelId = 3;
    goodReq.msg.DataSize = 1;
    goodReq.msg.Data[0] = 0x11;
    writeFrame(bridge.toChildWrite, Function::PassThruWriteMsgs, &goodReq, sizeof(goodReq));
    FrameHeader header{};
    PassThruWriteMsgsResponse goodResp{};
    assert(readFrame(bridge.fromChildRead, header, &goodResp, sizeof(goodResp)));
    assert(goodResp.result == STATUS_NOERROR);

    PassThruWriteMsgsRequest badReq = goodReq;
    badReq.msg.Data[0] = 0x99;
    writeFrame(bridge.toChildWrite, Function::PassThruWriteMsgs, &badReq, sizeof(badReq));
    PassThruWriteMsgsResponse badResp{};
    assert(readFrame(bridge.fromChildRead, header, &badResp, sizeof(badResp)));
    assert(badResp.result == ERR_FAILED);

    std::printf("test_write_msgs_success_and_failure: PASS\n");
}

void test_ioctl_read_vbatt(BridgeProcess& bridge)
{
    PassThruIoctlRequest req{};
    req.channelId = 3;
    req.ioctlId = READ_VBATT;
    writeFrame(bridge.toChildWrite, Function::PassThruIoctl, &req, sizeof(req));
    FrameHeader header{};
    PassThruIoctlResponse resp{};
    assert(readFrame(bridge.fromChildRead, header, &resp, sizeof(resp)));
    assert(resp.result == STATUS_NOERROR);
    assert(resp.vbatt == 12500);

    std::printf("test_ioctl_read_vbatt: PASS\n");
}

void test_child_crash_is_detected_as_broken_pipe(const std::string& hostExe, const std::string& dllPath)
{
    BridgeProcess bridge;
    assert(bridge.start(hostExe, dllPath));

    TerminateProcess(bridge.pi.hProcess, 1);
    WaitForSingleObject(bridge.pi.hProcess, 2000);

    PassThruCloseRequest req{};
    req.deviceId = 7;
    writeFrame(bridge.toChildWrite, Function::PassThruClose, &req, sizeof(req));
    FrameHeader header{};
    PassThruCloseResponse resp{};
    bool ok = readFrame(bridge.fromChildRead, header, &resp, sizeof(resp));
    assert(!ok && "readFrame should report failure once the child process is dead");

    CloseHandle(bridge.toChildWrite);
    CloseHandle(bridge.fromChildRead);
    CloseHandle(bridge.pi.hProcess);
    CloseHandle(bridge.pi.hThread);

    std::printf("test_child_crash_is_detected_as_broken_pipe: PASS\n");
}

} // namespace

int main(int argc, char **argv)
{
    // The host exe and fake DLL paths are 32-bit artifacts built outside
    // Bazel (scripts/compile-x86-bridge-artifacts.ps1 -- this Bazel setup has
    // no registered x86 Windows C++ toolchain), so CI passes their paths in
    // via J2534_BRIDGE_HOST_EXE/FAKE_J2534_DLL_PATH environment variables
    // (--test_env in .github/workflows/pr.yml). argv is kept as a fallback
    // for running this binary manually outside CI.
    const char *hostExeEnv = std::getenv("J2534_BRIDGE_HOST_EXE");
    const char *dllPathEnv = std::getenv("FAKE_J2534_DLL_PATH");
    std::string hostExe, dllPath;
    if (hostExeEnv && dllPathEnv)
    {
        hostExe = hostExeEnv;
        dllPath = dllPathEnv;
    }
    else if (argc == 3)
    {
        hostExe = argv[1];
        dllPath = argv[2];
    }
    else
    {
        std::fprintf(stderr, "usage: set J2534_BRIDGE_HOST_EXE/FAKE_J2534_DLL_PATH, or pass "
                             "j2534_bridge_integration_test <host-exe-path> <fake-dll-path>\n");
        return 1;
    }

    BridgeProcess bridge;
    assert(bridge.start(hostExe, dllPath) && "failed to spawn j2534_bridge_host");

    test_open_connect_and_read(bridge);
    test_write_msgs_success_and_failure(bridge);
    test_ioctl_read_vbatt(bridge);
    bridge.stop();

    test_child_crash_is_detected_as_broken_pipe(hostExe, dllPath);

    std::printf("All j2534_bridge_integration tests passed.\n");
    return 0;
}
