#include <memory>
#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

#include <array>
#include <gtest/gtest.h>
#include <cstdint>
#include <cstdio>
#include <windows.h>

using namespace j2534_bridge;

TEST(J2534BridgeProtocol, round_trip_small_payload)
{
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    BOOL ok = CreatePipe(&readEnd, &writeEnd, &sa, 0);
    ASSERT_TRUE(ok && "CreatePipe failed");
    std::unique_ptr<void, decltype(&CloseHandle)> read_handle(readEnd, &CloseHandle);
    std::unique_ptr<void, decltype(&CloseHandle)> write_handle(writeEnd, &CloseHandle);

    PassThruCloseRequest req{};
    req.deviceId = 42;

    bool wrote = writeFrame(writeEnd, Function::PassThruClose, &req, sizeof(req));
    ASSERT_TRUE(wrote && "writeFrame failed");

    FrameHeader header{};
    PassThruCloseRequest received{};
    bool read = readFrame(readEnd, header, &received, sizeof(received));
    ASSERT_TRUE(read && "readFrame failed");
    ASSERT_TRUE(header.function == Function::PassThruClose);
    ASSERT_TRUE(header.payloadSize == sizeof(req));
    ASSERT_TRUE(received.deviceId == 42);

    std::printf("test_round_trip_small_payload: PASS\n");
}

TEST(J2534BridgeProtocol, read_fails_on_closed_pipe)
{
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    BOOL ok = CreatePipe(&readEnd, &writeEnd, &sa, 0);
    ASSERT_TRUE(ok && "CreatePipe failed");
    std::unique_ptr<void, decltype(&CloseHandle)> read_handle(readEnd, &CloseHandle);
    std::unique_ptr<void, decltype(&CloseHandle)> write_handle(writeEnd, &CloseHandle);
    write_handle.reset(); // simulate the helper process exiting mid-call

    FrameHeader header{};
    std::array<char, 16> buf{};
    bool read = readFrame(readEnd, header, buf.data(), static_cast<std::uint32_t>(buf.size()));
    ASSERT_TRUE(!read && "readFrame should fail on a broken pipe");

    std::printf("test_read_fails_on_closed_pipe: PASS\n");
}
