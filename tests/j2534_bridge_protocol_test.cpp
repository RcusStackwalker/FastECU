#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

#include <cassert>
#include <cstdio>
#include <windows.h>

using namespace j2534_bridge;

static void test_round_trip_small_payload()
{
    HANDLE readEnd, writeEnd;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    BOOL ok = CreatePipe(&readEnd, &writeEnd, &sa, 0);
    assert(ok && "CreatePipe failed");

    PassThruCloseRequest req{};
    req.deviceId = 42;

    bool wrote = writeFrame(writeEnd, Function::PassThruClose, &req, sizeof(req));
    assert(wrote && "writeFrame failed");

    FrameHeader header{};
    PassThruCloseRequest received{};
    bool read = readFrame(readEnd, header, &received, sizeof(received));
    assert(read && "readFrame failed");
    assert(header.function == Function::PassThruClose);
    assert(header.payloadSize == sizeof(req));
    assert(received.deviceId == 42);

    CloseHandle(readEnd);
    CloseHandle(writeEnd);
    std::printf("test_round_trip_small_payload: PASS\n");
}

static void test_read_fails_on_closed_pipe()
{
    HANDLE readEnd, writeEnd;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    BOOL ok = CreatePipe(&readEnd, &writeEnd, &sa, 0);
    assert(ok && "CreatePipe failed");
    CloseHandle(writeEnd); // simulate the helper process exiting mid-call

    FrameHeader header{};
    char buf[16];
    bool read = readFrame(readEnd, header, buf, sizeof(buf));
    assert(!read && "readFrame should fail on a broken pipe");

    CloseHandle(readEnd);
    std::printf("test_read_fails_on_closed_pipe: PASS\n");
}

int main()
{
    test_round_trip_small_payload();
    test_read_fails_on_closed_pipe();
    std::printf("All j2534_bridge_protocol tests passed.\n");
    return 0;
}
