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
    HANDLE read_end = nullptr, write_end = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    BOOL ok = CreatePipe(&read_end, &write_end, &sa, 0);
    ASSERT_TRUE(ok && "CreatePipe failed");
    std::unique_ptr<void, decltype(&CloseHandle)> read_handle(read_end, &CloseHandle);
    std::unique_ptr<void, decltype(&CloseHandle)> write_handle(write_end, &CloseHandle);

    PassThruCloseRequest req{};
    req.device_id = 42;

    bool wrote = writeFrame(write_end, Function::kPassThruClose, &req, sizeof(req));
    ASSERT_TRUE(wrote && "writeFrame failed");

    FrameHeader header{};
    PassThruCloseRequest received{};
    bool read = readFrame(read_end, header, &received, sizeof(received));
    ASSERT_TRUE(read && "readFrame failed");
    ASSERT_TRUE(header.function == Function::kPassThruClose);
    ASSERT_TRUE(header.payload_size == sizeof(req));
    ASSERT_TRUE(received.device_id == 42);

    std::printf("test_round_trip_small_payload: PASS\n");
}

TEST(J2534BridgeProtocol, read_fails_on_closed_pipe)
{
    HANDLE read_end = nullptr, write_end = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    BOOL ok = CreatePipe(&read_end, &write_end, &sa, 0);
    ASSERT_TRUE(ok && "CreatePipe failed");
    std::unique_ptr<void, decltype(&CloseHandle)> read_handle(read_end, &CloseHandle);
    std::unique_ptr<void, decltype(&CloseHandle)> write_handle(write_end, &CloseHandle);
    write_handle.reset(); // simulate the helper process exiting mid-call

    FrameHeader header{};
    std::array<char, 16> buf{};
    bool read = readFrame(read_end, header, buf.data(), static_cast<std::uint32_t>(buf.size()));
    ASSERT_TRUE(!read && "readFrame should fail on a broken pipe");

    std::printf("test_read_fails_on_closed_pipe: PASS\n");
}
