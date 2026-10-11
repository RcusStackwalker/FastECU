#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"
#include "src/backend/flash/ecu/testing/recording_can_flash_transport.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/mock_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::_;
using ::testing::AtLeast;
using ::testing::Contains;
using ::testing::DoAll;
using ::testing::ElementsAre;
using ::testing::Return;

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

constexpr std::uint32_t kRequestId = 0x7E1;
constexpr std::uint32_t kResponseId = 0x7E9;
constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;
constexpr std::string_view kReflashRecoveryWarning =
    "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still running and "
    "receiving commands!";

enum class UploadB6Reply
{
    kNoFrame,
    kTimeout,
    kShort,
    kMalformed,
    kWrongCanId,
    kNegative,
    kAdapterError,
    kCancelled,
    kDisconnected,
};

struct Case
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
    std::uint32_t kernel_address;
};

constexpr std::array<Case, 2> kCases{{
    {"sub_tcu_denso_sh7055_can", "SH7055", 0x00080000, 0xFFFF9000},
    {"sub_tcu_denso_sh7058_can", "SH7058", 0x00100000, 0xFFFF3000},
}};

struct BlockFixture
{
    std::uint32_t start;
    std::uint32_t length;
    std::uint32_t zero_crc;
};

// Hand-derived from revision-59f4e442's selected kFlashDevices[] rows. The CRC
// literals use the legacy 0x5AA5A55A polynomial over plaintext zero bytes;
// no production geometry or checksum helper participates in these fixtures.
constexpr std::array<BlockFixture, 16> kSh7055Blocks{{
    {0x00000000, 0x00001000, 0xF722EF49},
    {0x00001000, 0x00001000, 0xF722EF49},
    {0x00002000, 0x00001000, 0xF722EF49},
    {0x00003000, 0x00001000, 0xF722EF49},
    {0x00004000, 0x00001000, 0xF722EF49},
    {0x00005000, 0x00001000, 0xF722EF49},
    {0x00006000, 0x00001000, 0xF722EF49},
    {0x00007000, 0x00001000, 0xF722EF49},
    {0x00008000, 0x00008000, 0xE5FD2EA2},
    {0x00010000, 0x00010000, 0xDFEA015D},
    {0x00020000, 0x00010000, 0xDFEA015D},
    {0x00030000, 0x00010000, 0xDFEA015D},
    {0x00040000, 0x00010000, 0xDFEA015D},
    {0x00050000, 0x00010000, 0xDFEA015D},
    {0x00060000, 0x00010000, 0xDFEA015D},
    {0x00070000, 0x00010000, 0xDFEA015D},
}};

constexpr std::array<BlockFixture, 16> kSh7058Blocks{{
    {0x00000000, 0x00001000, 0xF722EF49},
    {0x00001000, 0x00001000, 0xF722EF49},
    {0x00002000, 0x00001000, 0xF722EF49},
    {0x00003000, 0x00001000, 0xF722EF49},
    {0x00004000, 0x00001000, 0xF722EF49},
    {0x00005000, 0x00001000, 0xF722EF49},
    {0x00006000, 0x00001000, 0xF722EF49},
    {0x00007000, 0x00001000, 0xF722EF49},
    {0x00008000, 0x00018000, 0xC85F0DED},
    {0x00020000, 0x00020000, 0xC39BE4A8},
    {0x00040000, 0x00020000, 0xC39BE4A8},
    {0x00060000, 0x00020000, 0xC39BE4A8},
    {0x00080000, 0x00020000, 0xC39BE4A8},
    {0x000A0000, 0x00020000, 0xC39BE4A8},
    {0x000C0000, 0x00020000, 0xC39BE4A8},
    {0x000E0000, 0x00020000, 0xC39BE4A8},
}};

std::span<const BlockFixture> BlocksFor(const Case& test_case)
{
    return test_case.mcu == "SH7055" ? std::span<const BlockFixture>(kSh7055Blocks)
                                     : std::span<const BlockFixture>(kSh7058Blocks);
}

// Deliberately NOT RecordingCanFlashTransport from
// //src/backend/flash/ecu/testing. That shared type unions only additions that
// are inert unless set. This decorator's restart_in_progress is set
// unconditionally in reset_connection() and then changes what configure() and
// open() do, its write() classifies timeline entries by payload byte, and its
// read() cancels after a kernel-start reply. Folding any of that into the
// shared type would change the petrol and diesel suites' behaviour.
class RecordingCanTransport final : public ICanFlashTransport
{
  public:
    Status ResetConnection() override
    {
        lifecycle.push_back("reset_connection");
        if (timeline != nullptr)
        {
            timeline->push_back("reset_connection");
        }
        ++reset_call_count;
        restart_in_progress = true;
        Status result = reset_result;
        if (result.has_value() && cancellation_on_reset != nullptr)
        {
            cancellation_on_reset->SetCancelled(true);
        }
        return result;
    }
    Status Configure(const Iso15765Config& config) override
    {
        lifecycle.push_back("configure");
        if (timeline != nullptr)
        {
            timeline->push_back("configure");
        }
        if (restart_in_progress)
        {
            restart_configs.push_back(config);
            if (!restart_configure_result.has_value())
            {
                return restart_configure_result;
            }
            if (cancellation_on_configure != nullptr)
            {
                cancellation_on_configure->SetCancelled(true);
            }
        }
        return scripted.Configure(config);
    }
    Status Open() override
    {
        lifecycle.push_back("open");
        if (timeline != nullptr)
        {
            timeline->push_back("open");
        }
        if (restart_in_progress && !restart_open_result.has_value())
        {
            return restart_open_result;
        }
        Status result = scripted.Open();
        if (result.has_value() && restart_in_progress && cancellation_on_open != nullptr)
        {
            cancellation_on_open->SetCancelled(true);
        }
        return result;
    }
    Status Close() override
    {
        lifecycle.push_back("close");
        if (timeline != nullptr)
        {
            timeline->push_back("close");
        }
        return scripted.Close();
    }
    void RequestUnblock() noexcept override
    {
        scripted.RequestUnblock();
    }
    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        writes.emplace_back(data.begin(), data.end());
        Status result = scripted.Write(data, cancellation);
        if (result.has_value() && timeline != nullptr)
        {
            timeline->push_back(data.size() > 4 && data[4] == 0x31                      ? "kernel_start_write"
                                : data.size() > 8 && data[4] == 0x7a && data[8] == 0x00 ? "kernel_id_write"
                                                                                        : "write");
        }
        if (result.has_value() && cancellation_to_trigger != nullptr && !cancel_prefix.empty() &&
            data.size() >= cancel_prefix.size() && std::equal(cancel_prefix.begin(), cancel_prefix.end(), data.begin()))
        {
            cancellation_to_trigger->SetCancelled(true);
        }
        return result;
    }
    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        read_timeouts.push_back(timeout);
        Result<std::optional<bytes::Bytes>> result = scripted.Read(timeout, cancellation);
        if (result.has_value() && timeline != nullptr)
        {
            timeline->push_back("read");
        }
        if (result.has_value() && cancellation_after_kernel_start_reply != nullptr && !writes.empty() &&
            writes.back().size() > 4 && writes.back()[4] == 0x31)
        {
            cancellation_after_kernel_start_reply->SetCancelled(true);
        }
        return result;
    }

    ScriptedCanFlashTransport scripted;
    Status reset_result;
    Status restart_configure_result;
    Status restart_open_result;
    int reset_call_count = 0;
    bool restart_in_progress = false;
    std::vector<Iso15765Config> restart_configs;
    std::vector<std::string> lifecycle;
    std::vector<bytes::Bytes> writes;
    std::vector<std::chrono::milliseconds> read_timeouts;
    FakeCancellationToken *cancellation_to_trigger = nullptr;
    FakeCancellationToken *cancellation_after_kernel_start_reply = nullptr;
    FakeCancellationToken *cancellation_on_reset = nullptr;
    FakeCancellationToken *cancellation_on_configure = nullptr;
    FakeCancellationToken *cancellation_on_open = nullptr;
    bytes::Bytes cancel_prefix;
    std::vector<std::string> *timeline = nullptr;
};

class CancellingEventSink final : public RecordingEventSink
{
  public:
    explicit CancellingEventSink(FakeCancellationToken& cancellation) : cancellation_(cancellation)
    {
    }

    void Log(LogLevel level, std::string_view message) override
    {
        RecordingEventSink::Log(level, message);
        if (message == " erased")
        {
            cancellation_.SetCancelled(true);
        }
    }

  private:
    FakeCancellationToken& cancellation_;
};

KernelImage KernelFor(const Case& test_case, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = "tcu-denso-kernel", .load_address = test_case.kernel_address, .bytes = std::move(data)};
}

Result<FlashPlan> ReadPlan(const Case& test_case, bytes::Bytes kernel = {0x01, 0x02, 0x03, 0x04})
{
    return BuildSubaruTcuDensoSh705xCanPlan(FlashOperation::kRead, test_case.protocol, test_case.mcu, std::nullopt,
                                            KernelFor(test_case, std::move(kernel)));
}

Result<FlashPlan> WritePlan(const Case& test_case, bytes::Bytes image = {})
{
    if (image.empty())
    {
        image.assign(test_case.rom_size, bytes::Byte{0});
    }
    return BuildSubaruTcuDensoSh705xCanPlan(FlashOperation::kWrite, test_case.protocol, test_case.mcu, std::move(image),
                                            KernelFor(test_case));
}

bytes::Bytes Request(bytes::ByteView pdu)
{
    return ComposeBe(kRequestId, pdu);
}

bytes::Bytes Response(bytes::ByteView pdu)
{
    return ComposeBe(kResponseId, pdu);
}

// Legacy request_kernel_id(), r59f4e442 lines 1583-1647. This is the initial
// ISO-15765 probe's exact twelve-byte wire buffer, not a UDS PDU.
bytes::Bytes KernelIdRequest()
{
    return {0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

bytes::Bytes BeefRequest(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = ComposeBe(kRequestId, std::uint16_t{0xBEEF}, static_cast<std::uint16_t>(payload.size() + 1),
                                     bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes BeefResponse(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = ComposeBe(kResponseId, std::uint16_t{0xBEEF}, static_cast<std::uint16_t>(payload.size() + 1),
                                     bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes KernelIdResponse(bool with_transport_padding = false)
{
    bytes::Bytes response = BeefResponse(0x41, bytes::Bytes{'K', 'I', 'D'});
    if (with_transport_padding)
    {
        response.insert(response.end(), {0x00, 0x00});
    }
    return response;
}

void ConfigureAndOpen(SubaruTcuDensoSh705xCanExecutor& executor, const FlashPlan& plan, ICanFlashTransport& transport)
{
    auto setup = executor.TransportSetup(plan);
    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    ASSERT_TRUE(transport.Configure(*setup).has_value());
    ASSERT_TRUE(transport.Open().has_value());
}

void ScriptKernelAlive(ScriptedCanFlashTransport& transport, bool with_transport_padding = false)
{
    transport.ExpectWrite(KernelIdRequest());
    transport.QueueRead(KernelIdResponse(with_transport_padding));
    transport.QueueNoFrame(); // terminate the legacy short trailing drain
}

void ScriptKernelAliveFragmented(ScriptedCanFlashTransport& transport)
{
    transport.ExpectWrite(KernelIdRequest());
    // The declared BEEF body is four bytes (opcode plus KID), but the first
    // raw CAN envelope carries only the first payload byte.  The remaining
    // payload arrives in a second raw envelope, followed by an empty short
    // read that terminates the legacy trailing drain.
    transport.QueueRead(
        ComposeBe(kResponseId, std::uint16_t{0xBEEF}, std::uint16_t{4}, bytes::Byte{0x41}, bytes::Byte{'K'}));
    transport.QueueRead(Response(bytes::Bytes{'I', 'D'}));
    transport.QueueError(ErrorKind::kTimeout, "legacy short drain expired");
}

void ScriptKernelProbeTimeout(ScriptedCanFlashTransport& transport)
{
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        transport.ExpectWrite(KernelIdRequest());
        transport.QueueNoFrame();
    }
}

void ScriptReadPages(ScriptedCanFlashTransport& transport, std::uint32_t size, bytes::Byte wire_fill = 0)
{
    for (std::uint32_t address = 0; address < size; address += kReadPageSize)
    {
        transport.ExpectWrite(BeefRequest(0x03, ComposeBe(0x00_b, U24(address), std::uint16_t{kReadPageSize})));
        transport.QueueRead(BeefResponse(0x43, bytes::Bytes(kReadPageSize, wire_fill)));
    }
}

void ScriptRawReadPagesWithBoundarySentinels(ScriptedCanFlashTransport& transport, std::uint32_t size)
{
    constexpr std::array<bytes::Byte, 8> kFirstWireBytes{0xD3, 0x5A, 0xC7, 0x19, 0x2E, 0xF4, 0x80, 0x6B};
    constexpr std::array<bytes::Byte, 8> kLastWireBytes{0x9C, 0x31, 0xE7, 0x04, 0xB2, 0x6D, 0x58, 0xAF};
    for (std::uint32_t address = 0; address < size; address += kReadPageSize)
    {
        transport.ExpectWrite(BeefRequest(0x03, ComposeBe(0x00_b, U24(address), std::uint16_t{kReadPageSize})));
        bytes::Bytes page(kReadPageSize, bytes::Byte{0});
        if (address == 0)
        {
            std::copy(kFirstWireBytes.begin(), kFirstWireBytes.end(), page.begin());
        }
        if (address + kReadPageSize == size)
        {
            std::copy(kLastWireBytes.begin(), kLastWireBytes.end(), page.end() - kLastWireBytes.size());
        }
        transport.QueueRead(BeefResponse(0x43, page));
    }
}

void ScriptIdentityQueries(ScriptedCanFlashTransport& transport)
{
    // connect_bootloader(), r59f4e442 lines 137-214: both identity queries
    // are non-fatal, but successful read identity contributes to rom_id.
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xAA});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xEA, 0x00, 0x00, 0x00, 0x45, 0x43, 0x55, 0x30, 0x31});
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x09, 0x04});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x49, 0x04, 0x00, 0x43, 0x41, 0x4C});
}

void ScriptStrictSessionAndSecurity(ScriptedCanFlashTransport& transport)
{
    // connect_bootloader(), r59f4e442 lines 216-359. Seed 11 22 33 44 maps
    // to 35 B6 83 BF; the literal is fixed independently of executor code.
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x10, 0x03});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x50, 0x03});
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x27, 0x01});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x27, 0x02, 0x35, 0xB6, 0x83, 0xBF});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x67, 0x02});
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x10, 0x02});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x50, 0x42});
}

void QueueUploadB6Reply(ScriptedCanFlashTransport& transport, UploadB6Reply reply)
{
    switch (reply)
    {
    case UploadB6Reply::kNoFrame:
        transport.QueueNoFrame();
        return;
    case UploadB6Reply::kTimeout:
        transport.QueueError(ErrorKind::kTimeout, "legacy B6 timeout");
        return;
    case UploadB6Reply::kShort:
        transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07});
        return;
    case UploadB6Reply::kMalformed:
        transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE});
        return;
    case UploadB6Reply::kWrongCanId:
        transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0x76});
        return;
    case UploadB6Reply::kNegative:
        transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x7F, 0xB6, 0x31});
        return;
    case UploadB6Reply::kAdapterError:
        transport.QueueError(ErrorKind::kInternal, "legacy stale B6 adapter error");
        return;
    case UploadB6Reply::kCancelled:
        transport.QueueError(ErrorKind::kCancelled, "legacy B6 cancellation");
        return;
    case UploadB6Reply::kDisconnected:
        transport.QueueError(ErrorKind::kDisconnected, "legacy B6 disconnect");
        return;
    }
}

void ScriptKernelUploadUntilStart(ScriptedCanFlashTransport& transport, const Case& test_case,
                                  UploadB6Reply b6_reply = UploadB6Reply::kNoFrame)
{
    // upload_kernel(), r59f4e442 lines 369-627. Four kernel bytes pad to one
    // 128-byte transfer. The checksum word is 59 A3 A2 56, and the first and
    // last encrypted words are fixed here so the test never calls production
    // crypto helpers.
    transport.ExpectWrite(Request(ComposeBe(0x34_b, 0x04_b, 0x33_b, U24(test_case.kernel_address), U24(0x80))));
    transport.QueueRead(Response(bytes::Bytes{0x74, 0x20}));

    bytes::Bytes block{0xB6,
                       static_cast<bytes::Byte>(test_case.kernel_address >> 16U),
                       static_cast<bytes::Byte>(test_case.kernel_address >> 8U),
                       static_cast<bytes::Byte>(test_case.kernel_address),
                       0xA1,
                       0xB0,
                       0x3A,
                       0xDD};
    for (int word = 0; word < 30; ++word)
    {
        block.insert(block.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    block.insert(block.end(), {0x6C, 0x78, 0x52, 0x90});
    transport.ExpectWrite(Request(block));
    QueueUploadB6Reply(transport, b6_reply);
    transport.ExpectWrite(
        Request(bytes::Bytes{0xB6, static_cast<bytes::Byte>((test_case.kernel_address + 0x80U) >> 16U),
                             static_cast<bytes::Byte>((test_case.kernel_address + 0x80U) >> 8U),
                             static_cast<bytes::Byte>(test_case.kernel_address + 0x80U)}));
    QueueUploadB6Reply(transport, b6_reply); // The <= maxblocks loop emits one empty final block.

    transport.ExpectWrite(Request(bytes::Bytes{0x37}));
    transport.QueueRead(Response(bytes::Bytes{0x77}));
    transport.ExpectWrite(Request(bytes::Bytes{0x31, 0x01, 0x02, 0x02, 0x02}));
}

void ScriptKernelUpload(ScriptedCanFlashTransport& transport, const Case& test_case,
                        UploadB6Reply b6_reply = UploadB6Reply::kNoFrame,
                        bytes::Bytes start_reply = bytes::Bytes{0x71, 0x01, 0x02, 0x02, 0x02})
{
    ScriptKernelUploadUntilStart(transport, test_case, b6_reply);
    transport.QueueRead(Response(start_reply));
}

void ScriptHandDerived129ByteSh7055KernelUpload(ScriptedCanFlashTransport& transport)
{
    // Fixed transcript for 128 zero bytes followed by 01. Padding expands
    // the upload to 0x100 bytes. The second plaintext block begins with
    // 01 00 00 00 and ends with checksum 59 A5 A5 5A. Under the legacy
    // payload cipher those words are C0 41 D4 CA and 42 61 DB 2C;
    // encrypted zero words are E7 E2 14 30.
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x34, 0x04, 0x33, 0xFF, 0x90, 0x00, 0x00, 0x01, 0x00});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x74, 0x20});

    bytes::Bytes first_block{0x00, 0x00, 0x07, 0xE1, 0xB6, 0xFF, 0x90, 0x00};
    for (int word = 0; word < 32; ++word)
    {
        first_block.insert(first_block.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    transport.ExpectWrite(first_block);
    transport.QueueNoFrame();

    bytes::Bytes second_block{0x00, 0x00, 0x07, 0xE1, 0xB6, 0xFF, 0x90, 0x80, 0xC0, 0x41, 0xD4, 0xCA};
    for (int word = 0; word < 30; ++word)
    {
        second_block.insert(second_block.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    second_block.insert(second_block.end(), {0x42, 0x61, 0xDB, 0x2C});
    transport.ExpectWrite(second_block);
    transport.QueueNoFrame();

    // Legacy's <= block-count loop emits one final zero-length transfer.
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xB6, 0xFF, 0x91, 0x00});
    transport.QueueNoFrame();
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x37});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x77});
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x31, 0x01, 0x02, 0x02, 0x02});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x71, 0x01, 0x02, 0x02, 0x02});
}

bytes::Bytes ZeroChunk(std::size_t size)
{
    return bytes::Bytes(size, bytes::Byte{0});
}

void ScriptCrc(ScriptedCanFlashTransport& transport, const BlockFixture& block, std::uint32_t crc)
{
    transport.ExpectWrite(BeefRequest(0x02, ComposeBe(block.start, 0x00_b, U24(block.length))));
    transport.QueueRead(BeefResponse(0x42, ComposeBe(crc)));
    transport.QueueNoFrame(); // Legacy drains a short stale frame after each CRC comparison.
}

void ScriptCrcWithStale(ScriptedCanFlashTransport& transport, const BlockFixture& block, std::uint32_t crc,
                        bytes::ByteView stale)
{
    transport.ExpectWrite(BeefRequest(0x02, ComposeBe(block.start, 0x00_b, U24(block.length))));
    transport.QueueRead(BeefResponse(0x42, ComposeBe(crc)));
    transport.QueueRead(stale);
}

void ScriptCompare(ScriptedCanFlashTransport& transport, std::span<const BlockFixture> blocks,
                   bool first_block_mismatches)
{
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        const BlockFixture& block = blocks[index];
        ScriptCrc(transport, block, first_block_mismatches && index == 0 ? block.zero_crc ^ 1U : block.zero_crc);
    }
}

void ScriptCompareWithStaleCrcFrames(ScriptedCanFlashTransport& transport, std::span<const BlockFixture> blocks,
                                     bool first_block_mismatches)
{
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        const BlockFixture& block = blocks[index];
        const std::uint32_t crc = first_block_mismatches && index == 0 ? block.zero_crc ^ 1U : block.zero_crc;
        if (index == 0)
        {
            // Fixed wrong-ID stale frame after a mismatching CRC.
            ScriptCrcWithStale(transport, block, crc, bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xDE, 0xAD});
        }
        else if (index == 1)
        {
            // Fixed short junk after an equal CRC.
            ScriptCrcWithStale(transport, block, crc, bytes::Bytes{0x12, 0x34});
        }
        else
        {
            ScriptCrc(transport, block, crc);
        }
    }
}

void ScriptSh7058A5BlockCompare(ScriptedCanFlashTransport& transport, bool block_matches)
{
    // Block 8 is 0x18000 bytes at 0x8000. Its fixed CRC is AAA0B108 when
    // every byte is A5; all other image blocks remain all-zero fixtures.
    for (std::size_t index = 0; index < kSh7058Blocks.size(); ++index)
    {
        const std::uint32_t crc = index == 8 && block_matches ? 0xAAA0B108U : kSh7058Blocks[index].zero_crc;
        ScriptCrc(transport, kSh7058Blocks[index], crc);
    }
}

void ScriptFlashInit(ScriptedCanFlashTransport& transport)
{
    transport.ExpectWrite(BeefRequest(0x05));
    transport.QueueRead(BeefResponse(0x45, ComposeBe(std::uint32_t{0x00000200})));
    transport.ExpectWrite(BeefRequest(0x06));
    transport.QueueRead(BeefResponse(0x46, ComposeBe(std::uint32_t{0x00001000})));
    transport.ExpectWrite(BeefRequest(0x20));
    transport.QueueRead(BeefResponse(0x60));
}

void ScriptFirstFlashBlock(ScriptedCanFlashTransport& transport, bool include_chunks = true)
{
    transport.ExpectWrite(BeefRequest(0x04));
    transport.QueueRead(BeefResponse(0x44, bytes::Bytes{0x00, 0x64}));
    transport.ExpectWrite(BeefRequest(0x25, ComposeBe(std::uint32_t{0x00000000})));
    transport.QueueRead(BeefResponse(0x65));
    if (!include_chunks)
    {
        return;
    }
    const bytes::Bytes chunk = ZeroChunk(kWriteChunkSize);
    for (std::uint32_t offset = 0; offset < kCommitBlockSize; offset += kWriteChunkSize)
    {
        transport.ExpectWrite(BeefRequest(0x22, ComposeBe(offset, chunk)));
        transport.QueueRead(BeefResponse(0x62));
    }
    transport.ExpectWrite(
        BeefRequest(0x24, ComposeBe(std::uint32_t{0x00000000}, std::uint16_t{kCommitBlockSize}, 0xF722EF49U)));
    transport.QueueRead(BeefResponse(0x64));
}

void ScriptSh7058A5BlockWrite(ScriptedCanFlashTransport& transport)
{
    transport.ExpectWrite(BeefRequest(0x04));
    transport.QueueRead(BeefResponse(0x44, bytes::Bytes{0x00, 0x64}));
    transport.ExpectWrite(BeefRequest(0x25, ComposeBe(std::uint32_t{0x00008000})));
    transport.QueueRead(BeefResponse(0x65));

    const bytes::Bytes a5_chunk(kWriteChunkSize, bytes::Byte{0xA5});
    for (std::uint32_t address = 0x00008000; address < 0x00020000; address += kWriteChunkSize)
    {
        transport.ExpectWrite(BeefRequest(0x22, ComposeBe(address, a5_chunk)));
        transport.QueueRead(BeefResponse(0x62));
        if ((address + kWriteChunkSize) % kCommitBlockSize == 0)
        {
            const std::uint32_t commit_address = address + kWriteChunkSize - kCommitBlockSize;
            transport.ExpectWrite(BeefRequest(
                0x24, ComposeBe(commit_address, std::uint16_t{kCommitBlockSize}, std::uint32_t{0x958BA140})));
            transport.QueueRead(BeefResponse(0x64));
        }
    }
}

bool HasLog(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return std::any_of(events.logs.begin(), events.logs.end(), [level, text](const auto& log)
                       { return log.first == level && log.second.find(text) != std::string::npos; });
}

std::vector<std::string> LogMessagesStartingWith(const RecordingEventSink& events, std::string_view prefix)
{
    std::vector<std::string> messages;
    for (const auto& [level, message] : events.logs)
    {
        static_cast<void>(level);
        if (message.starts_with(prefix))
        {
            messages.push_back(message);
        }
    }
    return messages;
}

using LogRecord = std::pair<LogLevel, std::string>;

struct ExpectedPhaseProgress
{
    std::string_view name;
    int index;
    int count;
    int done;
    int total;
};

void ExpectExactPhaseProgress(const RecordingEventSink& events, const std::vector<ExpectedPhaseProgress>& expected)
{
    ASSERT_EQ(events.phase_progress_calls.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const RecordedPhaseProgress& actual = events.phase_progress_calls[index];
        const ExpectedPhaseProgress& want = expected[index];
        EXPECT_EQ(actual.phase_name, want.name) << index;
        EXPECT_EQ(actual.phase_index, want.index) << index;
        EXPECT_EQ(actual.phase_count, want.count) << index;
        EXPECT_EQ(actual.done, want.done) << index;
        EXPECT_EQ(actual.total, want.total) << index;
    }
}

void ExpectExactReadPhaseProgress(const RecordingEventSink& events, int total_bytes)
{
    const int pages = total_bytes / static_cast<int>(kReadPageSize);
    ASSERT_EQ(events.phase_progress_calls.size(), static_cast<std::size_t>(pages + 4));
    auto expect = [&events](std::size_t position, std::string_view name, int phase_index, int done, int total)
    {
        const RecordedPhaseProgress& actual = events.phase_progress_calls[position];
        EXPECT_EQ(actual.phase_name, name) << position;
        EXPECT_EQ(actual.phase_index, phase_index) << position;
        EXPECT_EQ(actual.phase_count, 2) << position;
        EXPECT_EQ(actual.done, done) << position;
        EXPECT_EQ(actual.total, total) << position;
    };
    expect(0, "Kernel", 1, 0, 1);
    expect(1, "Kernel", 1, 1, 1);
    expect(2, "Read", 2, 0, total_bytes);
    for (int page = 1; page < pages; ++page)
    {
        expect(static_cast<std::size_t>(page) + 2, "Read", 2, page * static_cast<int>(kReadPageSize), total_bytes);
    }
    expect(static_cast<std::size_t>(pages) + 2, "Read", 2, total_bytes - 1, total_bytes);
    expect(static_cast<std::size_t>(pages) + 3, "Read", 2, total_bytes, total_bytes);
}

void AppendCompareLogs(std::vector<LogRecord>& logs, std::span<const BlockFixture> blocks, bool first_block_mismatches,
                       bool after_reflash)
{
    logs.emplace_back(LogLevel::kInfo, after_reflash
                                           ? "--- Comparing ECU flash memory pages to image file after reflash ---"
                                           : "--- Comparing ECU flash memory pages to image file ---");
    logs.emplace_back(LogLevel::kInfo, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        const BlockFixture& block = blocks[index];
        const bool differs = first_block_mismatches && index == 0;
        const std::uint32_t image_crc = block.zero_crc;
        const std::uint32_t ecu_crc = differs ? block.zero_crc ^ 1U : block.zero_crc;
        logs.emplace_back(LogLevel::kInfo,
                          std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
        logs.emplace_back(LogLevel::kDebug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", ecu_crc, image_crc));
        logs.emplace_back(LogLevel::kInfo, std::format("\t{:08X}\t{:08X}", ecu_crc, image_crc));
        logs.emplace_back(LogLevel::kInfo, differs ? "\tNO" : "\tYES");
    }
    logs.emplace_back(LogLevel::kInfo, "Different blocks : ");
    if (first_block_mismatches)
    {
        logs.emplace_back(LogLevel::kInfo, "0, ");
    }
    logs.emplace_back(LogLevel::kInfo, first_block_mismatches ? " (total: 1)" : " (total: 0)");
}

std::vector<LogRecord> ExpectedWriteLogs(std::span<const BlockFixture> blocks)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::kInfo, "Checking if kernel is already running...");
    logs.emplace_back(LogLevel::kInfo, "Requesting kernel ID");
    logs.emplace_back(LogLevel::kInfo, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ");
    logs.emplace_back(LogLevel::kInfo, "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 ");
    logs.emplace_back(LogLevel::kInfo, "Kernel ID: KID");
    AppendCompareLogs(logs, blocks, true, false);
    logs.emplace_back(LogLevel::kInfo, "--- Start writing ROM file to ECU flash memory ---");
    logs.emplace_back(LogLevel::kInfo, "Check max message length");
    logs.emplace_back(LogLevel::kInfo, ": 0x0200");
    logs.emplace_back(LogLevel::kInfo, "Check flashblock size");
    logs.emplace_back(LogLevel::kInfo, ": 0x1000");
    logs.emplace_back(LogLevel::kInfo, "Test write mode off, perform actual flash write");
    logs.emplace_back(LogLevel::kError, "Flash mode succesfully set");
    logs.emplace_back(LogLevel::kInfo, "Flash block addr: 0x00000000 len: 0x00001000");
    logs.emplace_back(LogLevel::kInfo, "Check flash voltage");
    logs.emplace_back(LogLevel::kInfo, ": 2V");
    logs.emplace_back(LogLevel::kInfo, "Flash page erase addr: 0x00000000 len: 0x00001000");
    logs.emplace_back(LogLevel::kInfo, "Erasing flash page...");
    logs.emplace_back(LogLevel::kInfo, " erased");
    logs.emplace_back(LogLevel::kInfo, "Start flash write addr: 0x00000000 len: 0x00001000");
    constexpr std::array<std::uint32_t, 8> kBufferAddresses{0x00000000, 0x00000200, 0x00000400, 0x00000600,
                                                            0x00000800, 0x00000A00, 0x00000C00, 0x00000E00};
    constexpr std::array<unsigned, 8> kPercents{0, 12, 25, 37, 50, 62, 75, 87};
    for (std::size_t index = 0; index < kBufferAddresses.size(); ++index)
    {
        logs.emplace_back(LogLevel::kDebug, "Data written to flash buffer");
        logs.emplace_back(LogLevel::kInfo, std::format("Write flash buffer: 0x{:08X} ({}% - 512000 B/s, ~ 1 s)",
                                                       kBufferAddresses[index], kPercents[index]));
    }
    logs.emplace_back(LogLevel::kInfo, "Flash buffer write complete... ");
    logs.emplace_back(LogLevel::kDebug, "Image CRC32: 0xf722ef49");
    logs.emplace_back(LogLevel::kInfo, "Committ flash addr: 0x0");
    logs.emplace_back(LogLevel::kInfo, " len: 0x1000");
    logs.emplace_back(LogLevel::kInfo, " crc32: 0xf722ef49");
    logs.emplace_back(LogLevel::kInfo, "Flash block ok");
    logs.emplace_back(LogLevel::kInfo, "Block 0 reflash complete.");
    AppendCompareLogs(logs, blocks, false, true);
    return logs;
}

std::vector<LogRecord> ExpectedReadLogs(const Case& test_case)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::kInfo, "Checking if kernel is already running...");
    logs.emplace_back(LogLevel::kInfo, "Requesting kernel ID");
    logs.emplace_back(LogLevel::kInfo, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ");
    logs.emplace_back(LogLevel::kInfo, test_case.mcu == "SH7058"
                                           ? "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 00 00 "
                                           : "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 ");
    logs.emplace_back(LogLevel::kInfo, "Kernel ID: KID");
    logs.emplace_back(LogLevel::kInfo, "Start reading ROM, please wait...");
    constexpr unsigned kFakeClockSpeed = 1'024'000;
    for (std::uint32_t offset = 0; offset < test_case.rom_size; offset += kReadPageSize)
    {
        const unsigned time_left = ((test_case.rom_size - offset) / kFakeClockSpeed) % 9999U + 1U;
        logs.emplace_back(LogLevel::kInfo, std::format("Kernel read addr: 0x{:08X} length: 0x{:08X}, {:>6} B/s {:>6} s",
                                                       offset, kReadPageSize, kFakeClockSpeed, time_left));
    }
    logs.emplace_back(LogLevel::kInfo, "ROM read ready");
    return logs;
}

TEST(SubaruTcuDensoSh705xCanExecutor, TransportSetupUsesExactTcuIsoConfiguration)
{
    SubaruTcuDensoSh705xCanExecutor executor;
    for (const Case& test_case : kCases)
    {
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto setup = executor.TransportSetup(*plan);
        ASSERT_TRUE(setup.has_value()) << setup.error().detail;
        EXPECT_EQ(setup->bitrate, 500000);
        EXPECT_EQ(setup->request_id, kRequestId);
        EXPECT_EQ(setup->response_id, kResponseId);
        EXPECT_FALSE(setup->extended_id);
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, AlreadyRunningKernelReadsBothMcuGeometriesAndReturnsRawRom)
{
    SubaruTcuDensoSh705xCanExecutor executor;
    FakeCancellationToken cancellation;
    for (const Case& test_case : kCases)
    {
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        RecordingCanTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport.scripted, test_case.mcu == "SH7058");
        ScriptReadPages(transport.scripted, test_case.rom_size);
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << test_case.protocol << ": " << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_EQ(result->read_bytes->size(), test_case.rom_size);
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(4), ElementsAre(0x00, 0x00, 0x00, 0x00));
        EXPECT_FALSE(result->rom_id.has_value());
        EXPECT_TRUE(transport.scripted.ScriptConsumed());
        EXPECT_EQ(events.logs, ExpectedReadLogs(test_case));
        EXPECT_THAT(events.notices, ElementsAre("Reading ROM, please wait..."));
        ExpectExactReadPhaseProgress(events, static_cast<int>(test_case.rom_size));
        ASSERT_EQ(transport.writes.size(), 1U + test_case.rom_size / kReadPageSize);
        EXPECT_EQ(transport.writes.front(),
                  (bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        EXPECT_EQ(transport.writes[1], (bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00,
                                                     0x00, 0x00, 0x04, 0x00}));
        EXPECT_EQ(transport.writes.back(), test_case.mcu == "SH7055"
                                               ? (bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xBE, 0xEF, 0x00, 0x07, 0x03,
                                                               0x00, 0x07, 0xFC, 0x00, 0x04, 0x00})
                                               : (bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xBE, 0xEF, 0x00, 0x07, 0x03,
                                                               0x00, 0x0F, 0xFC, 0x00, 0x04, 0x00}));
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, ReadKeepsRawBeefBoundaryPayloadsForBothTcuGeometries)
{
    SubaruTcuDensoSh705xCanExecutor executor;
    FakeCancellationToken cancellation;
    for (const Case& test_case : kCases)
    {
        SCOPED_TRACE(test_case.protocol);
        auto plan = ReadPlan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptRawReadPagesWithBoundarySentinels(transport, test_case.rom_size);
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(8),
                    ElementsAre(0xD3, 0x5A, 0xC7, 0x19, 0x2E, 0xF4, 0x80, 0x6B));
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).last(8),
                    ElementsAre(0x9C, 0x31, 0xE7, 0x04, 0xB2, 0x6D, 0x58, 0xAF));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelIdCoalescesMultiplePortableCanPayloadFrames)
{
    auto plan = WritePlan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAliveFragmented(transport);
    ScriptCompare(transport, BlocksFor(kCases[1]), false);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelIdContinuationAppendsTheEntireAcceptedPayloadAndDrainsToEmpty)
{
    auto plan = WritePlan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    RecordingCanTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    transport.scripted.ExpectWrite(
        bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    transport.scripted.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x07, 0x41, 0x41});
    // These bytes happen to begin with BEEF, but this is a continuation
    // payload, not a second envelope. Legacy lines 1621-1625 append it whole.
    transport.scripted.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x03, 0x41, 0x42, 0x43});
    transport.scripted.QueueNoFrame();
    ScriptCompare(transport.scripted, BlocksFor(kCases[1]), false);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scripted.ScriptConsumed());
    ASSERT_GE(transport.read_timeouts.size(), 3U);
    EXPECT_THAT(std::span<const std::chrono::milliseconds>(transport.read_timeouts).first(3),
                ElementsAre(800ms, 200ms, 200ms));
    const std::string expected_kernel_id{"Kernel ID: A\xBE\xEF\x00\x03\x41", 17};
    EXPECT_THAT(events.logs, Contains(LogRecord{LogLevel::kInfo, expected_kernel_id}));
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelIdTrailingDrainPropagatesCancellationAndDisconnect)
{
    for (const ErrorKind error_kind : {ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        auto plan = WritePlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44});
        transport.QueueError(error_kind, "trailing drain interrupted");
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, error_kind);
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_THAT(LogMessagesStartingWith(events, "Kernel ID "),
                    ElementsAre("Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                                "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 "));
        ExpectExactPhaseProgress(events, {{"Kernel", 1, 4, 0, 1}});
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, InitialKernelIdTrailingDrainBoundFallsBackBeforeStartingAnotherCommand)
{
    auto plan = WritePlan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44});
    for (int fragment = 0; fragment < 32; ++fragment)
    {
        // Correct-ID envelope with an empty continuation payload. Exhausting
        // the portable bound must fail before a CRC command can reuse a
        // possibly contaminated transport queue.
        transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9});
    }
    ScriptIdentityQueries(transport);
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x10, 0x03});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x7F, 0x10, 0x22});
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_TRUE(HasLog(events, LogLevel::kInfo, "Requesting ECU ID"));
    EXPECT_TRUE(transport.ScriptConsumed());
    ExpectExactPhaseProgress(events, {{"Kernel", 1, 4, 0, 1}});
}

TEST(SubaruTcuDensoSh705xCanExecutor, InitialKernelProbeTreatsLegacyNonterminalRepliesAsKernelAbsent)
{
    enum class ProbeReply
    {
        kNoFrame,
        kTimeout,
        kAdapterError,
        kShort,
        kMalformedLength,
        kWrongCanId,
        kWrongOpcode,
    };
    for (const ProbeReply reply :
         {ProbeReply::kNoFrame, ProbeReply::kTimeout, ProbeReply::kAdapterError, ProbeReply::kShort,
          ProbeReply::kMalformedLength, ProbeReply::kWrongCanId, ProbeReply::kWrongOpcode})
    {
        SCOPED_TRACE(static_cast<int>(reply));
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        if (reply == ProbeReply::kNoFrame || reply == ProbeReply::kTimeout)
        {
            for (int attempt = 0; attempt < 5; ++attempt)
            {
                transport.ExpectWrite(KernelIdRequest());
                if (reply == ProbeReply::kNoFrame)
                {
                    transport.QueueNoFrame();
                }
                else
                {
                    transport.QueueError(ErrorKind::kTimeout, "legacy initial probe timeout");
                }
            }
        }
        else
        {
            transport.ExpectWrite(KernelIdRequest());
            switch (reply)
            {
            case ProbeReply::kAdapterError:
                transport.QueueError(ErrorKind::kInternal, "legacy initial probe adapter error");
                break;
            case ProbeReply::kShort:
                transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07});
                break;
            case ProbeReply::kMalformedLength:
                transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x02, 0x41});
                transport.QueueNoFrame();
                break;
            case ProbeReply::kWrongCanId:
                transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x01, 0x41});
                break;
            case ProbeReply::kWrongOpcode:
                transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01, 0x42});
                transport.QueueNoFrame();
                break;
            case ProbeReply::kNoFrame:
            case ProbeReply::kTimeout:
                break;
            }
        }
        // These literal identity exchanges prove fallback reached the next
        // ECU-initialization stage. A negative session response then bounds
        // the test without a kernel upload or ROM transcript.
        ScriptIdentityQueries(transport);
        transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x10, 0x03});
        transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x7F, 0x10, 0x22});
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(HasLog(events, LogLevel::kInfo, "Requesting ECU ID"));
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, InitialKernelProbeKeepsCancellationAndDisconnectTerminal)
{
    for (const ErrorKind terminal : {ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(static_cast<int>(terminal));
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectWrite(KernelIdRequest());
        transport.QueueError(terminal, "terminal initial probe outcome");
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, terminal);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, SessionSafetyCorrectionRejectsBothLegacyPartialMatchesAndAcceptsExactMatch)
{
    // Oracle line 230 uses `sid != 0x50 && subfunction != 0x03`; its truth
    // table accepts either 50 02 or 51 03 because one inequality is false.
    const std::array<bytes::Bytes, 2> partial_matches{{bytes::Bytes{0x50, 0x02}, bytes::Bytes{0x51, 0x03}}};
    for (const bytes::Bytes& reply : partial_matches)
    {
        SCOPED_TRACE(bytes::ToHex(reply));
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelProbeTimeout(transport);
        ScriptIdentityQueries(transport);
        transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x10, 0x03});
        transport.QueueRead(Response(reply));
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(transport.ScriptConsumed());
    }

    auto plan = ReadPlan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelProbeTimeout(transport);
    ScriptIdentityQueries(transport);
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x10, 0x03});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x50, 0x03});
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x27, 0x01});
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x7F, 0x27, 0x35});
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_FALSE(HasLog(events, LogLevel::kInfo, "Seed request ok"));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuDensoSh705xCanExecutor, UploadB6ReadsDiscardAllLegacyContentAndAdapterOutcomes)
{
    for (const UploadB6Reply reply :
         {UploadB6Reply::kNoFrame, UploadB6Reply::kTimeout, UploadB6Reply::kShort, UploadB6Reply::kMalformed,
          UploadB6Reply::kWrongCanId, UploadB6Reply::kNegative, UploadB6Reply::kAdapterError})
    {
        SCOPED_TRACE(static_cast<int>(reply));
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelProbeTimeout(transport);
        ScriptIdentityQueries(transport);
        ScriptStrictSessionAndSecurity(transport);
        ScriptKernelUpload(transport, kCases[1], reply);
        transport.ExpectWrite(KernelIdRequest());
        transport.QueueError(ErrorKind::kDisconnected, "bounded after every B6 read");
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kDisconnected);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, UploadB6KeepsCancellationAndDisconnectTerminal)
{
    for (const ErrorKind terminal : {ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(static_cast<int>(terminal));
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelProbeTimeout(transport);
        ScriptIdentityQueries(transport);
        ScriptStrictSessionAndSecurity(transport);
        ScriptKernelUploadUntilStart(transport, kCases[1],
                                     terminal == ErrorKind::kCancelled ? UploadB6Reply::kCancelled
                                                                       : UploadB6Reply::kDisconnected);
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, terminal);
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelStartAcceptsServiceOnlyAndFullEchoBeforeStrictPostUploadProbe)
{
    const std::array<bytes::Bytes, 2> accepted{{bytes::Bytes{0x71}, bytes::Bytes{0x71, 0x01, 0x02, 0x02, 0x02}}};
    for (const bytes::Bytes& start_reply : accepted)
    {
        SCOPED_TRACE(bytes::ToHex(start_reply));
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelProbeTimeout(transport);
        ScriptIdentityQueries(transport);
        ScriptStrictSessionAndSecurity(transport);
        ScriptKernelUpload(transport, kCases[1], UploadB6Reply::kNoFrame, start_reply);
        transport.ExpectWrite(KernelIdRequest());
        transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07});
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelStartRejectsWrongSidShortTimeoutCancellationAndDisconnect)
{
    struct StartCase
    {
        std::string_view name;
        std::optional<bytes::Bytes> frame;
        std::optional<ErrorKind> error;
        ErrorKind expected;
    };
    const std::array<StartCase, 5> cases{{
        {"wrong-sid", Response(bytes::Bytes{0x70}), std::nullopt, ErrorKind::kBadResponse},
        {"short", bytes::Bytes{0x00, 0x00, 0x07}, std::nullopt, ErrorKind::kBadResponse},
        {"timeout", std::nullopt, ErrorKind::kTimeout, ErrorKind::kTimeout},
        {"cancelled", std::nullopt, ErrorKind::kCancelled, ErrorKind::kCancelled},
        {"disconnected", std::nullopt, ErrorKind::kDisconnected, ErrorKind::kDisconnected},
    }};
    for (const StartCase& start : cases)
    {
        SCOPED_TRACE(start.name);
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelProbeTimeout(transport);
        ScriptIdentityQueries(transport);
        ScriptStrictSessionAndSecurity(transport);
        ScriptKernelUploadUntilStart(transport, kCases[1]);
        if (start.error.has_value())
        {
            transport.QueueError(*start.error, "kernel-start read outcome");
        }
        else
        {
            ASSERT_TRUE(start.frame.has_value());
            transport.QueueRead(*start.frame);
        }
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, start.expected);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, ProbeTimeoutRunsIdentityStrictUdsUploadAndPostUploadBeefRead)
{
    const Case& test_case = kCases[1];
    auto plan = ReadPlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelProbeTimeout(transport);
    ScriptIdentityQueries(transport);
    ScriptStrictSessionAndSecurity(transport);
    ScriptKernelUpload(transport, test_case);
    transport.ExpectWrite(KernelIdRequest());
    transport.QueueRead(KernelIdResponse());
    transport.QueueNoFrame();
    ScriptReadPages(transport, kReadPageSize);
    transport.ExpectWrite(BeefRequest(0x03, ComposeBe(0x00_b, U24(kReadPageSize), std::uint16_t{kReadPageSize})));
    transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01, 0x43});
    FakeCancellationToken cancellation;
    MockClock clock;
    EXPECT_CALL(clock, Sleep(50ms, _)).Times(AtLeast(1));
    EXPECT_CALL(clock, Sleep(500ms, _)).Times(AtLeast(1));
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_TRUE(HasLog(events, LogLevel::kInfo, "ECU ID: 4543553031"));
    EXPECT_TRUE(HasLog(events, LogLevel::kInfo, "CAL ID: CAL"));
    EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait...", "Reading ROM, please wait..."));
}

TEST(SubaruTcuDensoSh705xCanExecutor, ProbeTimeoutUploadsFixed129ByteKernelThenSuccessfullyReadsSh7055)
{
    bytes::Bytes kernel(129, bytes::Byte{0x00});
    kernel.back() = 0x01;
    auto plan = ReadPlan(kCases[0], std::move(kernel));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    RecordingCanTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelProbeTimeout(transport.scripted);
    ScriptIdentityQueries(transport.scripted);
    ScriptStrictSessionAndSecurity(transport.scripted);
    ScriptHandDerived129ByteSh7055KernelUpload(transport.scripted);
    transport.scripted.ExpectWrite(
        bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    transport.scripted.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44});
    transport.scripted.QueueNoFrame();
    ScriptReadPages(transport.scripted, 0x00080000);
    FakeCancellationToken cancellation;
    std::vector<std::string> timeline;
    transport.timeline = &timeline;
    MockClock clock;
    ON_CALL(clock, Sleep)
        .WillByDefault(DoAll([&](std::chrono::milliseconds duration, const ICancellationToken&)
                             { timeline.push_back(std::format("sleep:{}", duration.count())); }, clock.SleepOnFake()));
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), 0x00080000U);
    ASSERT_TRUE(result->rom_id.has_value());
    EXPECT_EQ(*result->rom_id, "CAL_4543553031_");
    ASSERT_EQ(transport.restart_configs.size(), 1U);
    EXPECT_EQ(transport.restart_configs.front().bitrate, 500000);
    EXPECT_EQ(transport.restart_configs.front().request_id, 0x7e1U);
    EXPECT_EQ(transport.restart_configs.front().response_id, 0x7e9U);
    EXPECT_FALSE(transport.restart_configs.front().extended_id);
    EXPECT_EQ(transport.reset_call_count, 1);
    const auto reset = std::ranges::find(timeline, "reset_connection");
    ASSERT_NE(reset, timeline.end());
    ASSERT_GE(std::distance(timeline.begin(), reset), 3);
    ASSERT_GE(std::distance(reset, timeline.end()), 7);
    EXPECT_EQ(*(reset - 3), "kernel_start_write");
    EXPECT_EQ(*(reset - 2), "sleep:50");
    EXPECT_EQ(*(reset - 1), "read");
    EXPECT_EQ(*reset, "reset_connection");
    EXPECT_EQ(*(reset + 1), "configure");
    EXPECT_EQ(*(reset + 2), "open");
    EXPECT_EQ(*(reset + 3), "sleep:500");
    EXPECT_EQ(*(reset + 4), "kernel_id_write");
    EXPECT_EQ(*(reset + 5), "sleep:100");
    EXPECT_EQ(*(reset + 6), "read");
    EXPECT_TRUE(transport.scripted.ScriptConsumed());
    EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait...", "Reading ROM, please wait..."));
    const std::vector<LogRecord> expected_upload_prefix{
        {LogLevel::kInfo, "Checking if kernel is already running..."},
        {LogLevel::kInfo, "Requesting kernel ID"},
        {LogLevel::kInfo, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::kInfo, "Kernel ID response: "},
        {LogLevel::kInfo, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::kInfo, "Kernel ID response: "},
        {LogLevel::kInfo, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::kInfo, "Kernel ID response: "},
        {LogLevel::kInfo, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::kInfo, "Kernel ID response: "},
        {LogLevel::kInfo, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::kInfo, "Kernel ID response: "},
        {LogLevel::kError, "No valid response from ECU"},
        {LogLevel::kInfo, "No response from kernel, initialising ECU..."},
        {LogLevel::kInfo, "Requesting ECU ID"},
        {LogLevel::kInfo, "ECU ID: 4543553031"},
        {LogLevel::kInfo, "Requesting CAL ID"},
        {LogLevel::kInfo, "CAL ID: CAL"},
        {LogLevel::kInfo, "Requesting session mode"},
        {LogLevel::kInfo, "Seed request ok"},
        {LogLevel::kInfo, "Sending seed key"},
        {LogLevel::kInfo, "Seed key ok"},
        {LogLevel::kInfo, "Requesting programming session"},
        {LogLevel::kInfo, "Succesfully set to programming session"},
        {LogLevel::kDebug, "Start address to upload kernel: 0xffff9000"},
        {LogLevel::kInfo, "Initialize kernel upload"},
        {LogLevel::kInfo, "Uploading kernel, please wait..."},
        {LogLevel::kInfo, "Kernel uploaded, starting..."},
        {LogLevel::kInfo, "Kernel started, initializing..."},
        {LogLevel::kInfo, "Requesting kernel ID"},
        {LogLevel::kInfo, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::kInfo, "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 "},
        {LogLevel::kInfo, "Kernel ID: KID"},
        {LogLevel::kInfo, "Start reading ROM, please wait..."},
    };
    ASSERT_GE(events.logs.size(), expected_upload_prefix.size());
    EXPECT_TRUE(std::equal(expected_upload_prefix.begin(), expected_upload_prefix.end(), events.logs.begin()));
    EXPECT_THAT(LogMessagesStartingWith(events, "Kernel ID "),
                ElementsAre("Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 "));
    ExpectExactReadPhaseProgress(events, 0x00080000);
}

TEST(SubaruTcuDensoSh705xCanExecutor, RestartResetConfigureAndOpenFailuresPropagateAndOuterCloseStillRuns)
{
    struct FailureCase
    {
        std::string_view stage;
        ErrorKind kind;
        std::vector<std::string> expected_lifecycle;
    };
    const std::array cases{
        FailureCase{"reset", ErrorKind::kInternal, {"configure", "open", "reset_connection", "close"}},
        FailureCase{
            "configure", ErrorKind::kInvalidConfig, {"configure", "open", "reset_connection", "configure", "close"}},
        FailureCase{
            "open", ErrorKind::kDisconnected, {"configure", "open", "reset_connection", "configure", "open", "close"}},
    };
    for (const FailureCase& failure : cases)
    {
        SCOPED_TRACE(failure.stage);
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto transport = std::make_unique<RecordingCanTransport>();
        RecordingCanTransport *observed = transport.get();
        ScriptKernelProbeTimeout(observed->scripted);
        ScriptIdentityQueries(observed->scripted);
        ScriptStrictSessionAndSecurity(observed->scripted);
        ScriptKernelUpload(observed->scripted, kCases[1]);
        if (failure.stage == "reset")
        {
            observed->reset_result = Fail(failure.kind, "restart reset marker");
        }
        else if (failure.stage == "configure")
        {
            observed->restart_configure_result = Fail(failure.kind, "restart configure marker");
        }
        else
        {
            observed->restart_open_result = Fail(failure.kind, "restart open marker");
        }
        FakeCancellationToken cancellation;
        MockClock clock;
        EXPECT_CALL(clock, Sleep(500ms, _)).Times(0);
        RecordingEventSink events;
        auto attempt = BindFlashAttempt(std::move(*plan), std::make_unique<SubaruTcuDensoSh705xCanExecutor>(),
                                        std::move(transport));

        const auto result = attempt->Run(clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, failure.kind);
        EXPECT_EQ(observed->lifecycle, failure.expected_lifecycle);
        EXPECT_EQ(observed->scripted.close_call_count, 1);
        EXPECT_TRUE(observed->scripted.ScriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, CancellationAfterKernelStartAndWithinRestartStopsAtTheNextBoundaryAndCloses)
{
    struct CancellationCase
    {
        std::string_view boundary;
        std::vector<std::string> expected_lifecycle;
    };
    const std::array cases{
        CancellationCase{"before-restart", {"configure", "open", "close"}},
        CancellationCase{"after-reset", {"configure", "open", "reset_connection", "close"}},
        CancellationCase{"after-configure", {"configure", "open", "reset_connection", "configure", "close"}},
        CancellationCase{"after-open", {"configure", "open", "reset_connection", "configure", "open", "close"}},
    };
    for (const CancellationCase& test_case : cases)
    {
        SCOPED_TRACE(test_case.boundary);
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto transport = std::make_unique<RecordingCanTransport>();
        RecordingCanTransport *observed = transport.get();
        ScriptKernelProbeTimeout(observed->scripted);
        ScriptIdentityQueries(observed->scripted);
        ScriptStrictSessionAndSecurity(observed->scripted);
        ScriptKernelUpload(observed->scripted, kCases[1]);
        FakeCancellationToken cancellation;
        if (test_case.boundary == "before-restart")
        {
            observed->cancellation_after_kernel_start_reply = &cancellation;
        }
        else if (test_case.boundary == "after-reset")
        {
            observed->cancellation_on_reset = &cancellation;
        }
        else if (test_case.boundary == "after-configure")
        {
            observed->cancellation_on_configure = &cancellation;
        }
        else
        {
            observed->cancellation_on_open = &cancellation;
        }
        MockClock clock;
        EXPECT_CALL(clock, Sleep(500ms, _)).Times(0);
        RecordingEventSink events;
        auto attempt = BindFlashAttempt(std::move(*plan), std::make_unique<SubaruTcuDensoSh705xCanExecutor>(),
                                        std::move(transport));

        const auto result = attempt->Run(clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_EQ(observed->lifecycle, test_case.expected_lifecycle);
        EXPECT_EQ(observed->scripted.close_call_count, 1);
        EXPECT_TRUE(observed->scripted.ScriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, CancellationDuringOrImmediatelyAfterRestartDelaySkipsStrictProbeAndCloses)
{
    for (const bool cancellation_returns_from_sleep : {false, true})
    {
        SCOPED_TRACE(cancellation_returns_from_sleep);
        auto plan = ReadPlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto transport = std::make_unique<RecordingCanTransport>();
        RecordingCanTransport *observed = transport.get();
        ScriptKernelProbeTimeout(observed->scripted);
        ScriptIdentityQueries(observed->scripted);
        ScriptStrictSessionAndSecurity(observed->scripted);
        ScriptKernelUpload(observed->scripted, kCases[1]);
        FakeCancellationToken cancellation;
        MockClock clock;
        if (cancellation_returns_from_sleep)
        {
            EXPECT_CALL(clock, Sleep(500ms, _))
                .WillOnce(DoAll(clock.SleepOnFake(), [&] { cancellation.SetCancelled(true); }, Return(Status{})));
        }
        else
        {
            EXPECT_CALL(clock, Sleep(500ms, _))
                .WillOnce(DoAll([&] { cancellation.SetCancelled(true); }, clock.SleepOnFake()));
        }
        RecordingEventSink events;
        auto attempt = BindFlashAttempt(std::move(*plan), std::make_unique<SubaruTcuDensoSh705xCanExecutor>(),
                                        std::move(transport));

        const auto result = attempt->Run(clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_EQ(observed->scripted.close_call_count, 1);
        EXPECT_TRUE(observed->scripted.ScriptConsumed());
        EXPECT_EQ(std::count_if(observed->writes.begin(), observed->writes.end(), [](const bytes::Bytes& write)
                                { return write.size() > 8 && write[4] == 0x7a && write[8] == 0x00; }),
                  5);
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelPhaseDoesNotCompleteWhenPostUploadIdentityDisconnects)
{
    auto plan = ReadPlan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelProbeTimeout(transport);
    ScriptIdentityQueries(transport);
    ScriptStrictSessionAndSecurity(transport);
    ScriptKernelUpload(transport, kCases[1]);
    transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    transport.QueueError(ErrorKind::kDisconnected, "adapter dropped during post-upload identity");
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kDisconnected);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait..."));
    ExpectExactPhaseProgress(events, {{"Kernel", 1, 2, 0, 1}});
}

TEST(SubaruTcuDensoSh705xCanExecutor, ZeroElapsedClockUsesOneMillisecondClampForExactReadAndWriteLogs)
{
    // The legacy read sampled an unstarted timer. A deterministic zero
    // elapsed interval is instead clamped to 1 ms while retaining its exact
    // rate/time formula and text layout.
    {
        auto plan = ReadPlan(kCases[0]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptReadPages(transport, kReadPageSize);
        FakeCancellationToken cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Read", kReadPageSize);

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_THAT(LogMessagesStartingWith(events, "Kernel read addr:"),
                    ElementsAre("Kernel read addr: 0x00000000 length: 0x00000400, 1024000 B/s      1 s"));
    }

    // The legacy writer logged uninitialized speed/time values for its first
    // window. The same 1 ms rule yields the exact stable first record below.
    {
        auto plan = WritePlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptCompare(transport, BlocksFor(kCases[1]), true);
        ScriptFlashInit(transport);
        transport.ExpectWrite(BeefRequest(0x04));
        transport.QueueRead(BeefResponse(0x44, bytes::Bytes{0x00, 0x64}));
        transport.ExpectWrite(BeefRequest(0x25, ComposeBe(std::uint32_t{0x00000000})));
        transport.QueueRead(BeefResponse(0x65));
        transport.ExpectWrite(BeefRequest(0x22, ComposeBe(std::uint32_t{0}, ZeroChunk(kWriteChunkSize))));
        transport.QueueRead(BeefResponse(0x62));
        FakeCancellationToken cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Write", kWriteChunkSize);

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_THAT(LogMessagesStartingWith(events, "Write flash buffer:"),
                    ElementsAre("Write flash buffer: 0x00000000 (0% - 512000 B/s, ~ 1 s)"));
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, WriteComparesErasesProgramsAndVerifiesFirstChangedBlockWithStaleCrcFrames)
{
    const Case& test_case = kCases[1];
    auto plan = WritePlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    ScriptCompareWithStaleCrcFrames(transport, BlocksFor(test_case), true);
    ScriptFlashInit(transport);
    ScriptFirstFlashBlock(transport);
    ScriptCompare(transport, BlocksFor(test_case), false);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(events.logs, ExpectedWriteLogs(BlocksFor(test_case)));
    EXPECT_THAT(events.notices, ElementsAre("Writing ROM, please wait..."));
    ExpectExactPhaseProgress(
        events, {{"Kernel", 1, 4, 0, 1},          {"Kernel", 1, 4, 1, 1},          {"Compare", 2, 4, 0, 16},
                 {"Compare", 2, 4, 1, 16},        {"Compare", 2, 4, 2, 16},        {"Compare", 2, 4, 3, 16},
                 {"Compare", 2, 4, 4, 16},        {"Compare", 2, 4, 5, 16},        {"Compare", 2, 4, 6, 16},
                 {"Compare", 2, 4, 7, 16},        {"Compare", 2, 4, 8, 16},        {"Compare", 2, 4, 9, 16},
                 {"Compare", 2, 4, 10, 16},       {"Compare", 2, 4, 11, 16},       {"Compare", 2, 4, 12, 16},
                 {"Compare", 2, 4, 13, 16},       {"Compare", 2, 4, 14, 16},       {"Compare", 2, 4, 15, 16},
                 {"Compare", 2, 4, 16, 16},       {"Write", 3, 4, 0, 0x1000},      {"Write", 3, 4, 0x0200, 0x1000},
                 {"Write", 3, 4, 0x0400, 0x1000}, {"Write", 3, 4, 0x0600, 0x1000}, {"Write", 3, 4, 0x0800, 0x1000},
                 {"Write", 3, 4, 0x0A00, 0x1000}, {"Write", 3, 4, 0x0C00, 0x1000}, {"Write", 3, 4, 0x0E00, 0x1000},
                 {"Write", 3, 4, 0x0FFF, 0x1000}, {"Write", 3, 4, 0x1000, 0x1000}, {"Complete", 4, 4, 0, 1},
                 {"Complete", 4, 4, 1, 1}});
}

TEST(SubaruTcuDensoSh705xCanExecutor, NonzeroLargeBlockUsesEveryWriteWindowAndExactOrderedProgress)
{
    bytes::Bytes image(kCases[1].rom_size, bytes::Byte{0x00});
    std::fill(image.begin() + 0x00008000, image.begin() + 0x00020000, bytes::Byte{0xA5});
    auto plan = WritePlan(kCases[1], std::move(image));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    RecordingCanTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport.scripted);
    ScriptSh7058A5BlockCompare(transport.scripted, false);
    ScriptFlashInit(transport.scripted);
    ScriptSh7058A5BlockWrite(transport.scripted);
    ScriptSh7058A5BlockCompare(transport.scripted, true);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scripted.ScriptConsumed());
    EXPECT_THAT(events.notices, ElementsAre("Writing ROM, please wait..."));
    EXPECT_FALSE(HasLog(events, LogLevel::kError, "*** ERROR IN FLASH PROCESS ***"));
    EXPECT_TRUE(HasLog(events, LogLevel::kDebug, "ROM CRC: 0xaaa0b108 IMG CRC: 0xaaa0b108"));

    // Kernel 0/1, Compare 0..16, Write start + 191 ordinary windows +
    // clamped last window + completion, then Complete 0/1.
    ASSERT_EQ(events.phase_progress_calls.size(), 215U);
    auto expect_phase = [&events](std::size_t index, std::string_view name, int phase_index, int done, int total)
    {
        const RecordedPhaseProgress& event = events.phase_progress_calls[index];
        EXPECT_EQ(event.phase_name, name) << index;
        EXPECT_EQ(event.phase_index, phase_index) << index;
        EXPECT_EQ(event.phase_count, 4) << index;
        EXPECT_EQ(event.done, done) << index;
        EXPECT_EQ(event.total, total) << index;
    };
    expect_phase(0, "Kernel", 1, 0, 1);
    expect_phase(1, "Kernel", 1, 1, 1);
    for (int done = 0; done <= 16; ++done)
    {
        expect_phase(static_cast<std::size_t>(done) + 2, "Compare", 2, done, 16);
    }
    expect_phase(19, "Write", 3, 0, 0x18000);
    for (int window = 1; window <= 191; ++window)
    {
        expect_phase(19 + static_cast<std::size_t>(window), "Write", 3, window * 0x200, 0x18000);
    }
    expect_phase(211, "Write", 3, 0x17FFF, 0x18000);
    expect_phase(212, "Write", 3, 0x18000, 0x18000);
    expect_phase(213, "Complete", 4, 0, 1);
    expect_phase(214, "Complete", 4, 1, 1);

    ASSERT_EQ(transport.writes.size(), 254U);
    // The first and last of all 24 fixed 0x1000 commits pin address, length,
    // and the independently calculated CRC for 0x1000 A5 bytes.
    EXPECT_EQ(transport.writes[30], (bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xBE, 0xEF, 0x00, 0x0B, 0x24, 0x00, 0x00,
                                                  0x80, 0x00, 0x10, 0x00, 0x95, 0x8B, 0xA1, 0x40}));
    EXPECT_EQ(transport.writes[237], (bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xBE, 0xEF, 0x00, 0x0B, 0x24, 0x00, 0x01,
                                                   0xF0, 0x00, 0x10, 0x00, 0x95, 0x8B, 0xA1, 0x40}));
    for (std::size_t page = 0; page < 24; ++page)
    {
        EXPECT_EQ(transport.writes[30 + page * 9][8], 0x24) << page;
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, UnchangedWriteEmitsExactFourPhaseProgressStream)
{
    const Case& test_case = kCases[1];
    auto plan = WritePlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    ScriptCompare(transport, BlocksFor(test_case), false);
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.ScriptConsumed());
    ExpectExactPhaseProgress(events, {{"Kernel", 1, 4, 0, 1},    {"Kernel", 1, 4, 1, 1},    {"Compare", 2, 4, 0, 16},
                                      {"Compare", 2, 4, 1, 16},  {"Compare", 2, 4, 2, 16},  {"Compare", 2, 4, 3, 16},
                                      {"Compare", 2, 4, 4, 16},  {"Compare", 2, 4, 5, 16},  {"Compare", 2, 4, 6, 16},
                                      {"Compare", 2, 4, 7, 16},  {"Compare", 2, 4, 8, 16},  {"Compare", 2, 4, 9, 16},
                                      {"Compare", 2, 4, 10, 16}, {"Compare", 2, 4, 11, 16}, {"Compare", 2, 4, 12, 16},
                                      {"Compare", 2, 4, 13, 16}, {"Compare", 2, 4, 14, 16}, {"Compare", 2, 4, 15, 16},
                                      {"Compare", 2, 4, 16, 16}, {"Write", 3, 4, 0, 0},     {"Complete", 4, 4, 0, 1},
                                      {"Complete", 4, 4, 1, 1}});
}

TEST(SubaruTcuDensoSh705xCanExecutor, CrcDrainIgnoresMalformedAndWrongIdFramesForEqualAndMismatchResults)
{
    const std::array<bytes::Bytes, 2> stale_frames{
        {bytes::Bytes{0x12, 0x34}, bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xDE, 0xAD}}};
    for (const bytes::Bytes& stale : stale_frames)
    {
        for (const bool mismatch : {false, true})
        {
            auto plan = WritePlan(kCases[1]);
            ASSERT_TRUE(plan.has_value()) << plan.error().detail;
            SubaruTcuDensoSh705xCanExecutor executor;
            ScriptedCanFlashTransport transport;
            ConfigureAndOpen(executor, *plan, transport);
            ScriptKernelAlive(transport);
            ScriptCrcWithStale(transport, kSh7058Blocks[0],
                               mismatch ? std::uint32_t{0xF722EF48} : std::uint32_t{0xF722EF49}, stale);
            for (std::size_t index = 1; index < kSh7058Blocks.size(); ++index)
            {
                ScriptCrc(transport, kSh7058Blocks[index], kSh7058Blocks[index].zero_crc);
            }
            FakeCancellationToken cancellation;
            FakeClock clock;
            PhaseCancellingEventSink events(cancellation, mismatch ? "Compare" : "never", 16);

            auto result = executor.Execute(*plan, transport, clock, cancellation, events);

            if (mismatch)
            {
                ASSERT_FALSE(result.has_value());
                EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
            }
            else
            {
                ASSERT_TRUE(result.has_value()) << result.error().detail;
            }
            EXPECT_TRUE(transport.ScriptConsumed());
            EXPECT_TRUE(HasLog(events, LogLevel::kInfo, mismatch ? "\tNO" : "\tYES"));
        }
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, CrcDrainPropagatesCancellationAndDisconnect)
{
    for (const ErrorKind error_kind : {ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        auto plan = WritePlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        transport.ExpectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xBE, 0xEF, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x10, 0x00});
        transport.QueueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x05, 0x42, 0xF7, 0x22, 0xEF, 0x49});
        transport.QueueError(error_kind, "stale drain interrupted");
        FakeCancellationToken cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, error_kind);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, CancellationInterruptsUploadReadCrcAndWriteLoops)
{
    // Upload loop: cancellation immediately after the first fixed B6 write
    // must prevent Kernel completion and propagate from the pending read.
    {
        bytes::Bytes kernel(129, bytes::Byte{0x00});
        kernel.back() = 0x01;
        auto plan = ReadPlan(kCases[0], std::move(kernel));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        RecordingCanTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelProbeTimeout(transport.scripted);
        ScriptIdentityQueries(transport.scripted);
        ScriptStrictSessionAndSecurity(transport.scripted);
        ScriptHandDerived129ByteSh7055KernelUpload(transport.scripted);
        FakeCancellationToken cancellation;
        transport.cancellation_to_trigger = &cancellation;
        transport.cancel_prefix = bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xB6};
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait..."));
        ExpectExactPhaseProgress(events, {{"Kernel", 1, 2, 0, 1}});
    }

    // Read loop: one complete 0x400 page is reported, then the next boundary
    // observes cancellation before sending another request.
    {
        auto plan = ReadPlan(kCases[0]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptReadPages(transport, kReadPageSize);
        FakeCancellationToken cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Read", kReadPageSize);

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Read");
        EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(kReadPageSize));
    }

    // CRC loop: cancellation on the first completed comparison is observed
    // by the legacy 5 ms pacing checkpoint before block 2.
    {
        auto plan = WritePlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptCrc(transport, kSh7058Blocks[0], 0xF722EF49);
        FakeCancellationToken cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Compare", 1);

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Compare");
        EXPECT_EQ(events.phase_progress_calls.back().done, 1);
    }

    // Write loop: cancellation after one 0x200 buffer update stops before
    // the second window and retains the post-erase recovery warning.
    {
        auto plan = WritePlan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelAlive(transport);
        ScriptCompare(transport, BlocksFor(kCases[1]), true);
        ScriptFlashInit(transport);
        transport.ExpectWrite(BeefRequest(0x04));
        transport.QueueRead(BeefResponse(0x44, bytes::Bytes{0x00, 0x64}));
        transport.ExpectWrite(BeefRequest(0x25, ComposeBe(std::uint32_t{0x00000000})));
        transport.QueueRead(BeefResponse(0x65));
        transport.ExpectWrite(BeefRequest(0x22, ComposeBe(std::uint32_t{0}, ZeroChunk(kWriteChunkSize))));
        transport.QueueRead(BeefResponse(0x62));
        FakeCancellationToken cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Write", kWriteChunkSize);

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
        EXPECT_TRUE(transport.ScriptConsumed());
        EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Write");
        EXPECT_EQ(events.phase_progress_calls.back().done, static_cast<int>(kWriteChunkSize));
        EXPECT_TRUE(HasLog(events, LogLevel::kError, kReflashRecoveryWarning));
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, RejectsInvalidPlanBeforeTransportInteraction)
{
    auto fields = FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kSubaruTcuDensoSh705xCan,
        .transport = TransportKind::kCanIso15765,
        .target_id = std::string(kCases[0].protocol),
        .mcu_name = std::string(kCases[0].mcu),
        .transfer_region = {0, kCases[0].rom_size},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelFor(kCases[0]),
        .family_plan = SubaruTcuDensoSh705xCanPlan{.request_id = 0x7E0},
        .confirmations = {},
    };
    auto plan = ValidateAndBuild(std::move(fields));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kInvalidConfig);
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

TEST(SubaruTcuDensoSh705xCanExecutor, NegativeAndDisconnectRepliesAreTyped)
{
    auto plan = ReadPlan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    FakeCancellationToken cancellation;

    {
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        ScriptKernelProbeTimeout(transport);
        ScriptIdentityQueries(transport);
        transport.ExpectWrite(Request(bytes::Bytes{0x10, 0x03}));
        transport.QueueRead(Response(bytes::Bytes{0x7F, 0x10, 0x22}));
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    }
    {
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        ConfigureAndOpen(executor, *plan, transport);
        transport.ExpectWrite(KernelIdRequest());
        transport.QueueError(ErrorKind::kDisconnected, "adapter dropped");
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::kDisconnected);
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, CancellationAfterEraseReturnsRecoveryWarning)
{
    const Case& test_case = kCases[1];
    auto plan = WritePlan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    ConfigureAndOpen(executor, *plan, transport);
    ScriptKernelAlive(transport);
    ScriptCompare(transport, BlocksFor(test_case), true);
    ScriptFlashInit(transport);
    ScriptFirstFlashBlock(transport, false);
    FakeCancellationToken cancellation;
    FakeClock clock;
    CancellingEventSink events(cancellation);

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
    EXPECT_TRUE(HasLog(events, LogLevel::kError, kReflashRecoveryWarning));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruTcuDensoSh705xCanExecutor, BoundAttemptReturnsCloseErrorOnlyWhenExecutionSucceeds)
{
    auto plan = ReadPlan(kCases[0]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    auto success_transport = std::make_unique<ScriptedCanFlashTransport>();
    ScriptKernelAlive(*success_transport);
    ScriptReadPages(*success_transport, kCases[0].rom_size);
    success_transport->close_result = Fail(ErrorKind::kDisconnected, "close failed");
    auto success_attempt =
        BindFlashAttempt(*plan, std::make_unique<SubaruTcuDensoSh705xCanExecutor>(), std::move(success_transport));
    FakeCancellationToken cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto close_only = success_attempt->Run(clock, cancellation, events);

    ASSERT_FALSE(close_only.has_value());
    EXPECT_EQ(close_only.error().kind, ErrorKind::kDisconnected);

    auto failing_transport = std::make_unique<ScriptedCanFlashTransport>();
    ScriptKernelAlive(*failing_transport);
    failing_transport->ExpectWrite(BeefRequest(0x03, ComposeBe(0x00_b, U24(0), std::uint16_t{kReadPageSize})));
    failing_transport->QueueNoFrame();
    failing_transport->close_result = Fail(ErrorKind::kInternal, "close also failed");
    auto failing_attempt =
        BindFlashAttempt(*plan, std::make_unique<SubaruTcuDensoSh705xCanExecutor>(), std::move(failing_transport));
    RecordingEventSink failing_events;

    auto execution_error = failing_attempt->Run(clock, cancellation, failing_events);

    ASSERT_FALSE(execution_error.has_value());
    EXPECT_EQ(execution_error.error().kind, ErrorKind::kTimeout);
    EXPECT_TRUE(HasLog(failing_events, LogLevel::kWarning, "close failed after execution error"));
}

} // namespace
} // namespace fastecu::flash
