#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::Contains;
using ::testing::ElementsAre;

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;

constexpr std::uint32_t kRequestId = 0x7E1;
constexpr std::uint32_t kResponseId = 0x7E9;
constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;
constexpr std::string_view kReflashRecoveryWarning =
    "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still running and "
    "receiving commands!";

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

// Hand-derived from revision-59f4e442's selected flashdevices[] rows. The CRC
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

std::span<const BlockFixture> blocks_for(const Case& test_case)
{
    return test_case.mcu == "SH7055" ? std::span<const BlockFixture>(kSh7055Blocks)
                                     : std::span<const BlockFixture>(kSh7058Blocks);
}

class NeverCancelled final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

class ToggleCancellation final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return cancelled_.load();
    }
    void cancel()
    {
        cancelled_.store(true);
    }

  private:
    std::atomic_bool cancelled_{false};
};

class RecordingClock final : public FakeClock
{
  public:
    Status sleep(int ms, const ICancellationToken& cancellation) override
    {
        sleeps.push_back(ms);
        return FakeClock::sleep(ms, cancellation);
    }

    std::vector<int> sleeps;
};

class RecordingCanTransport final : public ICanFlashTransport
{
  public:
    Status configure(const Iso15765Config& config) override
    {
        return scripted.configure(config);
    }
    Status open() override
    {
        return scripted.open();
    }
    Status close() override
    {
        return scripted.close();
    }
    void request_unblock() noexcept override
    {
        scripted.request_unblock();
    }
    Status write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        writes.emplace_back(data.begin(), data.end());
        Status result = scripted.write(data, cancellation);
        if (result.has_value() && cancellation_to_trigger != nullptr && !cancel_prefix.empty() &&
            data.size() >= cancel_prefix.size() && std::equal(cancel_prefix.begin(), cancel_prefix.end(), data.begin()))
        {
            cancellation_to_trigger->cancel();
        }
        return result;
    }
    Result<std::optional<bytes::Bytes>> read(int timeout_ms, const ICancellationToken& cancellation) override
    {
        read_timeouts.push_back(timeout_ms);
        return scripted.read(timeout_ms, cancellation);
    }

    ScriptedCanFlashTransport scripted;
    std::vector<bytes::Bytes> writes;
    std::vector<int> read_timeouts;
    ToggleCancellation *cancellation_to_trigger = nullptr;
    bytes::Bytes cancel_prefix;
};

class PhaseCancellingEventSink final : public RecordingEventSink
{
  public:
    PhaseCancellingEventSink(ToggleCancellation& cancellation, std::string phase, int done)
        : cancellation_(cancellation), phase_(std::move(phase)), done_(done)
    {
    }

    void phase_progress(const PhaseProgressEvent& event) override
    {
        RecordingEventSink::phase_progress(event);
        if (event.phase_name == phase_ && event.done == done_)
        {
            cancellation_.cancel();
        }
    }

  private:
    ToggleCancellation& cancellation_;
    std::string phase_;
    int done_;
};

class CancellingEventSink final : public RecordingEventSink
{
  public:
    explicit CancellingEventSink(ToggleCancellation& cancellation) : cancellation_(cancellation)
    {
    }

    void log(LogLevel level, std::string_view message) override
    {
        RecordingEventSink::log(level, message);
        if (message == " erased")
        {
            cancellation_.cancel();
        }
    }

  private:
    ToggleCancellation& cancellation_;
};

KernelImage kernel_for(const Case& test_case, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = "tcu-denso-kernel", .load_address = test_case.kernel_address, .bytes = std::move(data)};
}

Result<FlashPlan> read_plan(const Case& test_case, bytes::Bytes kernel = {0x01, 0x02, 0x03, 0x04})
{
    return build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::Read, test_case.protocol, test_case.mcu, std::nullopt,
                                                  kernel_for(test_case, std::move(kernel)));
}

Result<FlashPlan> write_plan(const Case& test_case, bytes::Bytes image = {})
{
    if (image.empty())
    {
        image.assign(test_case.rom_size, bytes::Byte{0});
    }
    return build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::Write, test_case.protocol, test_case.mcu,
                                                  std::move(image), kernel_for(test_case));
}

bytes::Bytes request(bytes::ByteView pdu)
{
    return composeBe(kRequestId, pdu);
}

bytes::Bytes response(bytes::ByteView pdu)
{
    return composeBe(kResponseId, pdu);
}

// Legacy request_kernel_id(), r59f4e442 lines 1583-1647. This is the initial
// ISO-15765 probe's exact twelve-byte wire buffer, not a UDS PDU.
bytes::Bytes kernel_id_request()
{
    return {0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

bytes::Bytes beef_request(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = composeBe(kRequestId, std::uint16_t{0xBEEF}, static_cast<std::uint16_t>(payload.size() + 1),
                                     bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes beef_response(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes message = composeBe(kResponseId, std::uint16_t{0xBEEF}, static_cast<std::uint16_t>(payload.size() + 1),
                                     bytes::Byte(opcode));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

bytes::Bytes kernel_id_response(bool with_transport_padding = false)
{
    bytes::Bytes response = beef_response(0x41, bytes::Bytes{'K', 'I', 'D'});
    if (with_transport_padding)
    {
        response.insert(response.end(), {0x00, 0x00});
    }
    return response;
}

void configure_and_open(SubaruTcuDensoSh705xCanExecutor& executor, const FlashPlan& plan, ICanFlashTransport& transport)
{
    auto setup = executor.transport_setup(plan);
    ASSERT_TRUE(setup.has_value()) << setup.error().detail;
    ASSERT_TRUE(transport.configure(*setup).has_value());
    ASSERT_TRUE(transport.open().has_value());
}

void script_kernel_alive(ScriptedCanFlashTransport& transport, bool with_transport_padding = false)
{
    transport.expectWrite(kernel_id_request());
    transport.queueRead(kernel_id_response(with_transport_padding));
    transport.queue_no_frame(); // terminate the legacy short trailing drain
}

void script_kernel_alive_fragmented(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(kernel_id_request());
    // The declared BEEF body is four bytes (opcode plus KID), but the first
    // raw CAN envelope carries only the first payload byte.  The remaining
    // payload arrives in a second raw envelope, followed by an empty short
    // read that terminates the legacy trailing drain.
    transport.queueRead(
        composeBe(kResponseId, std::uint16_t{0xBEEF}, std::uint16_t{4}, bytes::Byte{0x41}, bytes::Byte{'K'}));
    transport.queueRead(response(bytes::Bytes{'I', 'D'}));
    transport.queue_error(ErrorKind::Timeout, "legacy short drain expired");
}

void script_kernel_probe_timeout(ScriptedCanFlashTransport& transport)
{
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        transport.expectWrite(kernel_id_request());
        transport.queue_no_frame();
    }
}

void script_read_pages(ScriptedCanFlashTransport& transport, std::uint32_t size, bytes::Byte wire_fill = 0)
{
    for (std::uint32_t address = 0; address < size; address += kReadPageSize)
    {
        transport.expectWrite(beef_request(0x03, composeBe(0x00_b, u24(address), std::uint16_t{kReadPageSize})));
        transport.queueRead(beef_response(0x43, bytes::Bytes(kReadPageSize, wire_fill)));
    }
}

void script_raw_read_pages_with_boundary_sentinels(ScriptedCanFlashTransport& transport, std::uint32_t size)
{
    constexpr std::array<bytes::Byte, 8> kFirstWireBytes{0xD3, 0x5A, 0xC7, 0x19, 0x2E, 0xF4, 0x80, 0x6B};
    constexpr std::array<bytes::Byte, 8> kLastWireBytes{0x9C, 0x31, 0xE7, 0x04, 0xB2, 0x6D, 0x58, 0xAF};
    for (std::uint32_t address = 0; address < size; address += kReadPageSize)
    {
        transport.expectWrite(beef_request(0x03, composeBe(0x00_b, u24(address), std::uint16_t{kReadPageSize})));
        bytes::Bytes page(kReadPageSize, bytes::Byte{0});
        if (address == 0)
        {
            std::copy(kFirstWireBytes.begin(), kFirstWireBytes.end(), page.begin());
        }
        if (address + kReadPageSize == size)
        {
            std::copy(kLastWireBytes.begin(), kLastWireBytes.end(), page.end() - kLastWireBytes.size());
        }
        transport.queueRead(beef_response(0x43, page));
    }
}

void script_identity_queries(ScriptedCanFlashTransport& transport)
{
    // connect_bootloader(), r59f4e442 lines 137-214: both identity queries
    // are non-fatal, but successful read identity contributes to rom_id.
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xAA});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xEA, 0x00, 0x00, 0x00, 0x45, 0x43, 0x55, 0x30, 0x31});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x09, 0x04});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x49, 0x04, 0x00, 0x43, 0x41, 0x4C});
}

void script_strict_session_and_security(ScriptedCanFlashTransport& transport)
{
    // connect_bootloader(), r59f4e442 lines 216-359. Seed 11 22 33 44 maps
    // to 35 B6 83 BF; the literal is fixed independently of executor code.
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x10, 0x03});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x50, 0x03});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x27, 0x01});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x67, 0x01, 0x11, 0x22, 0x33, 0x44});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x27, 0x02, 0x35, 0xB6, 0x83, 0xBF});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x67, 0x02});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x10, 0x02});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x50, 0x42});
}

void script_kernel_upload(ScriptedCanFlashTransport& transport, const Case& test_case)
{
    // upload_kernel(), r59f4e442 lines 369-627. Four kernel bytes pad to one
    // 128-byte transfer. The checksum word is 59 A3 A2 56, and the first and
    // last encrypted words are fixed here so the test never calls production
    // crypto helpers.
    transport.expectWrite(request(composeBe(0x34_b, 0x04_b, 0x33_b, u24(test_case.kernel_address), u24(0x80))));
    transport.queueRead(response(bytes::Bytes{0x74, 0x20}));

    bytes::Bytes block{0xB6,
                       static_cast<bytes::Byte>(test_case.kernel_address >> 16),
                       static_cast<bytes::Byte>(test_case.kernel_address >> 8),
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
    transport.expectWrite(request(block));
    transport.queue_no_frame(); // Legacy reads but does not validate transfer-block replies.
    transport.expectWrite(request(bytes::Bytes{0xB6, static_cast<bytes::Byte>((test_case.kernel_address + 0x80) >> 16),
                                               static_cast<bytes::Byte>((test_case.kernel_address + 0x80) >> 8),
                                               static_cast<bytes::Byte>(test_case.kernel_address + 0x80)}));
    transport.queue_no_frame(); // The <= maxblocks loop emits one empty final block.

    transport.expectWrite(request(bytes::Bytes{0x37}));
    transport.queueRead(response(bytes::Bytes{0x77}));
    transport.expectWrite(request(bytes::Bytes{0x31, 0x01, 0x02, 0x02, 0x02}));
    transport.queueRead(response(bytes::Bytes{0x71, 0x01, 0x02, 0x02, 0x02}));
}

void script_hand_derived_129_byte_sh7055_kernel_upload(ScriptedCanFlashTransport& transport)
{
    // Fixed transcript for 128 zero bytes followed by 01. Padding expands
    // the upload to 0x100 bytes. The second plaintext block begins with
    // 01 00 00 00 and ends with checksum 59 A5 A5 5A. Under the legacy
    // payload cipher those words are C0 41 D4 CA and 42 61 DB 2C;
    // encrypted zero words are E7 E2 14 30.
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x34, 0x04, 0x33, 0xFF, 0x90, 0x00, 0x00, 0x01, 0x00});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x74, 0x20});

    bytes::Bytes first_block{0x00, 0x00, 0x07, 0xE1, 0xB6, 0xFF, 0x90, 0x00};
    for (int word = 0; word < 32; ++word)
    {
        first_block.insert(first_block.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    transport.expectWrite(first_block);
    transport.queue_no_frame();

    bytes::Bytes second_block{0x00, 0x00, 0x07, 0xE1, 0xB6, 0xFF, 0x90, 0x80, 0xC0, 0x41, 0xD4, 0xCA};
    for (int word = 0; word < 30; ++word)
    {
        second_block.insert(second_block.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    second_block.insert(second_block.end(), {0x42, 0x61, 0xDB, 0x2C});
    transport.expectWrite(second_block);
    transport.queue_no_frame();

    // Legacy's <= block-count loop emits one final zero-length transfer.
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xB6, 0xFF, 0x91, 0x00});
    transport.queue_no_frame();
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x37});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x77});
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x31, 0x01, 0x02, 0x02, 0x02});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0x71, 0x01, 0x02, 0x02, 0x02});
}

bytes::Bytes zero_chunk(std::size_t size)
{
    return bytes::Bytes(size, bytes::Byte{0});
}

void script_crc(ScriptedCanFlashTransport& transport, const BlockFixture& block, std::uint32_t crc)
{
    transport.expectWrite(beef_request(0x02, composeBe(block.start, 0x00_b, u24(block.length))));
    transport.queueRead(beef_response(0x42, composeBe(crc)));
    transport.queue_no_frame(); // Legacy drains a short stale frame after each CRC comparison.
}

void script_crc_with_stale(ScriptedCanFlashTransport& transport, const BlockFixture& block, std::uint32_t crc,
                           bytes::ByteView stale)
{
    transport.expectWrite(beef_request(0x02, composeBe(block.start, 0x00_b, u24(block.length))));
    transport.queueRead(beef_response(0x42, composeBe(crc)));
    transport.queueRead(stale);
}

void script_compare(ScriptedCanFlashTransport& transport, std::span<const BlockFixture> blocks,
                    bool first_block_mismatches)
{
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        const BlockFixture& block = blocks[index];
        script_crc(transport, block, first_block_mismatches && index == 0 ? block.zero_crc ^ 1U : block.zero_crc);
    }
}

void script_compare_with_stale_crc_frames(ScriptedCanFlashTransport& transport, std::span<const BlockFixture> blocks,
                                          bool first_block_mismatches)
{
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        const BlockFixture& block = blocks[index];
        const std::uint32_t crc = first_block_mismatches && index == 0 ? block.zero_crc ^ 1U : block.zero_crc;
        if (index == 0)
        {
            // Fixed wrong-ID stale frame after a mismatching CRC.
            script_crc_with_stale(transport, block, crc, bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xDE, 0xAD});
        }
        else if (index == 1)
        {
            // Fixed short junk after an equal CRC.
            script_crc_with_stale(transport, block, crc, bytes::Bytes{0x12, 0x34});
        }
        else
        {
            script_crc(transport, block, crc);
        }
    }
}

void script_sh7058_a5_block_compare(ScriptedCanFlashTransport& transport, bool block_matches)
{
    // Block 8 is 0x18000 bytes at 0x8000. Its fixed CRC is AAA0B108 when
    // every byte is A5; all other image blocks remain all-zero fixtures.
    for (std::size_t index = 0; index < kSh7058Blocks.size(); ++index)
    {
        const std::uint32_t crc = index == 8 && block_matches ? 0xAAA0B108U : kSh7058Blocks[index].zero_crc;
        script_crc(transport, kSh7058Blocks[index], crc);
    }
}

void script_flash_init(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(beef_request(0x05));
    transport.queueRead(beef_response(0x45, composeBe(std::uint32_t{0x00000200})));
    transport.expectWrite(beef_request(0x06));
    transport.queueRead(beef_response(0x46, composeBe(std::uint32_t{0x00001000})));
    transport.expectWrite(beef_request(0x20));
    transport.queueRead(beef_response(0x60));
}

void script_first_flash_block(ScriptedCanFlashTransport& transport, bool include_chunks = true)
{
    transport.expectWrite(beef_request(0x04));
    transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.expectWrite(beef_request(0x25, composeBe(std::uint32_t{0x00000000})));
    transport.queueRead(beef_response(0x65));
    if (!include_chunks)
    {
        return;
    }
    const bytes::Bytes chunk = zero_chunk(kWriteChunkSize);
    for (std::uint32_t offset = 0; offset < kCommitBlockSize; offset += kWriteChunkSize)
    {
        transport.expectWrite(beef_request(0x22, composeBe(offset, chunk)));
        transport.queueRead(beef_response(0x62));
    }
    transport.expectWrite(
        beef_request(0x24, composeBe(std::uint32_t{0x00000000}, std::uint16_t{kCommitBlockSize}, 0xF722EF49U)));
    transport.queueRead(beef_response(0x64));
}

void script_sh7058_a5_block_write(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(beef_request(0x04));
    transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
    transport.expectWrite(beef_request(0x25, composeBe(std::uint32_t{0x00008000})));
    transport.queueRead(beef_response(0x65));

    const bytes::Bytes a5_chunk(kWriteChunkSize, bytes::Byte{0xA5});
    for (std::uint32_t address = 0x00008000; address < 0x00020000; address += kWriteChunkSize)
    {
        transport.expectWrite(beef_request(0x22, composeBe(address, a5_chunk)));
        transport.queueRead(beef_response(0x62));
        if ((address + kWriteChunkSize) % kCommitBlockSize == 0)
        {
            const std::uint32_t commit_address = address + kWriteChunkSize - kCommitBlockSize;
            transport.expectWrite(beef_request(
                0x24, composeBe(commit_address, std::uint16_t{kCommitBlockSize}, std::uint32_t{0x958BA140})));
            transport.queueRead(beef_response(0x64));
        }
    }
}

bool has_log(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return std::any_of(events.logs.begin(), events.logs.end(), [level, text](const auto& log)
                       { return log.first == level && log.second.find(text) != std::string::npos; });
}

std::vector<std::string> log_messages_starting_with(const RecordingEventSink& events, std::string_view prefix)
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

void expect_exact_phase_progress(const RecordingEventSink& events, const std::vector<ExpectedPhaseProgress>& expected)
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

void expect_exact_read_phase_progress(const RecordingEventSink& events, int total_bytes)
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
        expect(static_cast<std::size_t>(page + 2), "Read", 2, page * static_cast<int>(kReadPageSize), total_bytes);
    }
    expect(static_cast<std::size_t>(pages + 2), "Read", 2, total_bytes - 1, total_bytes);
    expect(static_cast<std::size_t>(pages + 3), "Read", 2, total_bytes, total_bytes);
}

void append_compare_logs(std::vector<LogRecord>& logs, std::span<const BlockFixture> blocks,
                         bool first_block_mismatches, bool after_reflash)
{
    logs.emplace_back(LogLevel::Info, after_reflash
                                          ? "--- Comparing ECU flash memory pages to image file after reflash ---"
                                          : "--- Comparing ECU flash memory pages to image file ---");
    logs.emplace_back(LogLevel::Info, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        const BlockFixture& block = blocks[index];
        const bool differs = first_block_mismatches && index == 0;
        const std::uint32_t image_crc = block.zero_crc;
        const std::uint32_t ecu_crc = differs ? block.zero_crc ^ 1U : block.zero_crc;
        logs.emplace_back(LogLevel::Info, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
        logs.emplace_back(LogLevel::Debug, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", ecu_crc, image_crc));
        logs.emplace_back(LogLevel::Info, std::format("\t{:08X}\t{:08X}", ecu_crc, image_crc));
        logs.emplace_back(LogLevel::Info, differs ? "\tNO" : "\tYES");
    }
    logs.emplace_back(LogLevel::Info, "Different blocks : ");
    if (first_block_mismatches)
    {
        logs.emplace_back(LogLevel::Info, "0, ");
    }
    logs.emplace_back(LogLevel::Info, first_block_mismatches ? " (total: 1)" : " (total: 0)");
}

std::vector<LogRecord> expected_write_logs(std::span<const BlockFixture> blocks)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::Info, "Checking if kernel is already running...");
    logs.emplace_back(LogLevel::Info, "Requesting kernel ID");
    logs.emplace_back(LogLevel::Info, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ");
    logs.emplace_back(LogLevel::Info, "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 ");
    logs.emplace_back(LogLevel::Info, "Kernel ID: KID");
    append_compare_logs(logs, blocks, true, false);
    logs.emplace_back(LogLevel::Info, "--- Start writing ROM file to ECU flash memory ---");
    logs.emplace_back(LogLevel::Info, "Check max message length");
    logs.emplace_back(LogLevel::Info, ": 0x0200");
    logs.emplace_back(LogLevel::Info, "Check flashblock size");
    logs.emplace_back(LogLevel::Info, ": 0x1000");
    logs.emplace_back(LogLevel::Info, "Test write mode off, perform actual flash write");
    logs.emplace_back(LogLevel::Error, "Flash mode succesfully set");
    logs.emplace_back(LogLevel::Info, "Flash block addr: 0x00000000 len: 0x00001000");
    logs.emplace_back(LogLevel::Info, "Check flash voltage");
    logs.emplace_back(LogLevel::Info, ": 2V");
    logs.emplace_back(LogLevel::Info, "Flash page erase addr: 0x00000000 len: 0x00001000");
    logs.emplace_back(LogLevel::Info, "Erasing flash page...");
    logs.emplace_back(LogLevel::Info, " erased");
    logs.emplace_back(LogLevel::Info, "Start flash write addr: 0x00000000 len: 0x00001000");
    constexpr std::array<std::uint32_t, 8> kBufferAddresses{0x00000000, 0x00000200, 0x00000400, 0x00000600,
                                                            0x00000800, 0x00000A00, 0x00000C00, 0x00000E00};
    constexpr std::array<unsigned, 8> kPercents{0, 12, 25, 37, 50, 62, 75, 87};
    for (std::size_t index = 0; index < kBufferAddresses.size(); ++index)
    {
        logs.emplace_back(LogLevel::Debug, "Data written to flash buffer");
        logs.emplace_back(LogLevel::Info, std::format("Write flash buffer: 0x{:08X} ({}% - 512000 B/s, ~ 1 s)",
                                                      kBufferAddresses[index], kPercents[index]));
    }
    logs.emplace_back(LogLevel::Info, "Flash buffer write complete... ");
    logs.emplace_back(LogLevel::Debug, "Image CRC32: 0xf722ef49");
    logs.emplace_back(LogLevel::Info, "Committ flash addr: 0x0");
    logs.emplace_back(LogLevel::Info, " len: 0x1000");
    logs.emplace_back(LogLevel::Info, " crc32: 0xf722ef49");
    logs.emplace_back(LogLevel::Info, "Flash block ok");
    logs.emplace_back(LogLevel::Info, "Block 0 reflash complete.");
    append_compare_logs(logs, blocks, false, true);
    return logs;
}

std::vector<LogRecord> expected_read_logs(const Case& test_case)
{
    std::vector<LogRecord> logs;
    logs.emplace_back(LogLevel::Info, "Checking if kernel is already running...");
    logs.emplace_back(LogLevel::Info, "Requesting kernel ID");
    logs.emplace_back(LogLevel::Info, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ");
    logs.emplace_back(LogLevel::Info, test_case.mcu == "SH7058"
                                          ? "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 00 00 "
                                          : "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 ");
    logs.emplace_back(LogLevel::Info, "Kernel ID: KID");
    logs.emplace_back(LogLevel::Info, "Start reading ROM, please wait...");
    constexpr unsigned kFakeClockSpeed = 1'024'000;
    for (std::uint32_t offset = 0; offset < test_case.rom_size; offset += kReadPageSize)
    {
        const unsigned time_left = ((test_case.rom_size - offset) / kFakeClockSpeed) % 9999U + 1U;
        logs.emplace_back(LogLevel::Info, std::format("Kernel read addr: 0x{:08X} length: 0x{:08X}, {:>6} B/s {:>6} s",
                                                      offset, kReadPageSize, kFakeClockSpeed, time_left));
    }
    logs.emplace_back(LogLevel::Info, "ROM read ready");
    return logs;
}

TEST(SubaruTcuDensoSh705xCanExecutor, TransportSetupUsesExactTcuIsoConfiguration)
{
    SubaruTcuDensoSh705xCanExecutor executor;
    for (const Case& test_case : kCases)
    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto setup = executor.transport_setup(*plan);
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
    NeverCancelled cancellation;
    for (const Case& test_case : kCases)
    {
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport.scripted, test_case.mcu == "SH7058");
        script_read_pages(transport.scripted, test_case.rom_size);
        RecordingClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << test_case.protocol << ": " << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_EQ(result->read_bytes->size(), test_case.rom_size);
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(4), ElementsAre(0x00, 0x00, 0x00, 0x00));
        EXPECT_FALSE(result->rom_id.has_value());
        EXPECT_TRUE(transport.scripted.scriptConsumed());
        EXPECT_EQ(events.logs, expected_read_logs(test_case));
        EXPECT_THAT(events.notices, ElementsAre("Reading ROM, please wait..."));
        expect_exact_read_phase_progress(events, static_cast<int>(test_case.rom_size));
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
    NeverCancelled cancellation;
    for (const Case& test_case : kCases)
    {
        SCOPED_TRACE(test_case.protocol);
        auto plan = read_plan(test_case);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_raw_read_pages_with_boundary_sentinels(transport, test_case.rom_size);
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_TRUE(result.has_value()) << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).first(8),
                    ElementsAre(0xD3, 0x5A, 0xC7, 0x19, 0x2E, 0xF4, 0x80, 0x6B));
        EXPECT_THAT(bytes::ByteView(*result->read_bytes).last(8),
                    ElementsAre(0x9C, 0x31, 0xE7, 0x04, 0xB2, 0x6D, 0x58, 0xAF));
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelIdCoalescesMultiplePortableCanPayloadFrames)
{
    auto plan = write_plan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive_fragmented(transport);
    script_compare(transport, blocks_for(kCases[1]), false);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelIdContinuationAppendsTheEntireAcceptedPayloadAndDrainsToEmpty)
{
    auto plan = write_plan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    transport.scripted.expectWrite(
        bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    transport.scripted.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x07, 0x41, 0x41});
    // These bytes happen to begin with BEEF, but this is a continuation
    // payload, not a second envelope. Legacy lines 1621-1625 append it whole.
    transport.scripted.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x03, 0x41, 0x42, 0x43});
    transport.scripted.queue_no_frame();
    script_compare(transport.scripted, blocks_for(kCases[1]), false);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    ASSERT_GE(transport.read_timeouts.size(), 3U);
    EXPECT_THAT(std::span<const int>(transport.read_timeouts).first(3), ElementsAre(800, 200, 200));
    const std::string expected_kernel_id{"Kernel ID: A\xBE\xEF\x00\x03\x41", 17};
    EXPECT_THAT(events.logs, Contains(LogRecord{LogLevel::Info, expected_kernel_id}));
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelIdTrailingDrainPropagatesCancellationAndDisconnect)
{
    for (const ErrorKind error_kind : {ErrorKind::Cancelled, ErrorKind::Disconnected})
    {
        auto plan = write_plan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44});
        transport.queue_error(error_kind, "trailing drain interrupted");
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, error_kind);
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_THAT(log_messages_starting_with(events, "Kernel ID "),
                    ElementsAre("Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                                "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 "));
        expect_exact_phase_progress(events, {{"Kernel", 1, 4, 0, 1}});
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelIdTrailingDrainBoundFailsBeforeStartingAnotherCommand)
{
    auto plan = write_plan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44});
    for (int fragment = 0; fragment < 32; ++fragment)
    {
        // Correct-ID envelope with an empty continuation payload. Exhausting
        // the portable bound must fail before a CRC command can reuse a
        // possibly contaminated transport queue.
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9});
    }
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(transport.writesConsumed(), 1U);
    EXPECT_TRUE(transport.scriptConsumed());
    expect_exact_phase_progress(events, {{"Kernel", 1, 4, 0, 1}});
}

TEST(SubaruTcuDensoSh705xCanExecutor, ProbeTimeoutRunsIdentityStrictUdsUploadAndPostUploadBeefRead)
{
    const Case& test_case = kCases[1];
    auto plan = read_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport);
    script_identity_queries(transport);
    script_strict_session_and_security(transport);
    script_kernel_upload(transport, test_case);
    transport.expectWrite(kernel_id_request());
    transport.queueRead(kernel_id_response());
    transport.queue_no_frame();
    script_read_pages(transport, kReadPageSize);
    transport.expectWrite(beef_request(0x03, composeBe(0x00_b, u24(kReadPageSize), std::uint16_t{kReadPageSize})));
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01, 0x43});
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_THAT(clock.sleeps, Contains(50));
    EXPECT_THAT(clock.sleeps, Contains(500));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "ECU ID: 4543553031"));
    EXPECT_TRUE(has_log(events, LogLevel::Info, "CAL ID: CAL"));
    EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait...", "Reading ROM, please wait..."));
}

TEST(SubaruTcuDensoSh705xCanExecutor, ProbeTimeoutUploadsFixed129ByteKernelThenSuccessfullyReadsSh7055)
{
    bytes::Bytes kernel(129, bytes::Byte{0x00});
    kernel.back() = 0x01;
    auto plan = read_plan(kCases[0], std::move(kernel));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport.scripted);
    script_identity_queries(transport.scripted);
    script_strict_session_and_security(transport.scripted);
    script_hand_derived_129_byte_sh7055_kernel_upload(transport.scripted);
    transport.scripted.expectWrite(
        bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    transport.scripted.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x04, 0x41, 0x4B, 0x49, 0x44});
    transport.scripted.queue_no_frame();
    script_read_pages(transport.scripted, 0x00080000);
    NeverCancelled cancellation;
    RecordingClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), 0x00080000U);
    ASSERT_TRUE(result->rom_id.has_value());
    EXPECT_EQ(*result->rom_id, "CAL_4543553031_");
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait...", "Reading ROM, please wait..."));
    const std::vector<LogRecord> expected_upload_prefix{
        {LogLevel::Info, "Checking if kernel is already running..."},
        {LogLevel::Info, "Requesting kernel ID"},
        {LogLevel::Info, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::Info, "Kernel ID response: "},
        {LogLevel::Info, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::Info, "Kernel ID response: "},
        {LogLevel::Info, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::Info, "Kernel ID response: "},
        {LogLevel::Info, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::Info, "Kernel ID response: "},
        {LogLevel::Info, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::Info, "Kernel ID response: "},
        {LogLevel::Error, "No valid response from ECU"},
        {LogLevel::Info, "No response from kernel, initialising ECU..."},
        {LogLevel::Info, "Requesting ECU ID"},
        {LogLevel::Info, "ECU ID: 4543553031"},
        {LogLevel::Info, "Requesting CAL ID"},
        {LogLevel::Info, "CAL ID: CAL"},
        {LogLevel::Info, "Requesting session mode"},
        {LogLevel::Info, "Seed request ok"},
        {LogLevel::Info, "Sending seed key"},
        {LogLevel::Info, "Seed key ok"},
        {LogLevel::Info, "Requesting programming session"},
        {LogLevel::Info, "Succesfully set to programming session"},
        {LogLevel::Debug, "Start address to upload kernel: 0xffff9000"},
        {LogLevel::Info, "Initialize kernel upload"},
        {LogLevel::Info, "Uploading kernel, please wait..."},
        {LogLevel::Info, "Kernel uploaded, starting..."},
        {LogLevel::Info, "Kernel started, initializing..."},
        {LogLevel::Info, "Requesting kernel ID"},
        {LogLevel::Info, "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 "},
        {LogLevel::Info, "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 "},
        {LogLevel::Info, "Kernel ID: KID"},
        {LogLevel::Info, "Start reading ROM, please wait..."},
    };
    ASSERT_GE(events.logs.size(), expected_upload_prefix.size());
    EXPECT_TRUE(std::equal(expected_upload_prefix.begin(), expected_upload_prefix.end(), events.logs.begin()));
    EXPECT_THAT(log_messages_starting_with(events, "Kernel ID "),
                ElementsAre("Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: ", "Kernel ID request: 00 00 07 e1 7a a0 00 00 00 00 00 00 ",
                            "Kernel ID response: 00 00 07 e9 be ef 00 04 41 4b 49 44 "));
    expect_exact_read_phase_progress(events, 0x00080000);
}

TEST(SubaruTcuDensoSh705xCanExecutor, KernelPhaseDoesNotCompleteWhenPostUploadIdentityDisconnects)
{
    auto plan = read_plan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_probe_timeout(transport);
    script_identity_queries(transport);
    script_strict_session_and_security(transport);
    script_kernel_upload(transport, kCases[1]);
    transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    transport.queue_error(ErrorKind::Disconnected, "adapter dropped during post-upload identity");
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait..."));
    expect_exact_phase_progress(events, {{"Kernel", 1, 2, 0, 1}});
}

TEST(SubaruTcuDensoSh705xCanExecutor, ZeroElapsedClockUsesOneMillisecondClampForExactReadAndWriteLogs)
{
    // The legacy read sampled an unstarted timer. A deterministic zero
    // elapsed interval is instead clamped to 1 ms while retaining its exact
    // rate/time formula and text layout.
    {
        auto plan = read_plan(kCases[0]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_read_pages(transport, kReadPageSize);
        ToggleCancellation cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Read", kReadPageSize);

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_THAT(log_messages_starting_with(events, "Kernel read addr:"),
                    ElementsAre("Kernel read addr: 0x00000000 length: 0x00000400, 1024000 B/s      1 s"));
    }

    // The legacy writer logged uninitialized speed/time values for its first
    // window. The same 1 ms rule yields the exact stable first record below.
    {
        auto plan = write_plan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, blocks_for(kCases[1]), true);
        script_flash_init(transport);
        transport.expectWrite(beef_request(0x04));
        transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
        transport.expectWrite(beef_request(0x25, composeBe(std::uint32_t{0x00000000})));
        transport.queueRead(beef_response(0x65));
        transport.expectWrite(beef_request(0x22, composeBe(std::uint32_t{0}, zero_chunk(kWriteChunkSize))));
        transport.queueRead(beef_response(0x62));
        ToggleCancellation cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Write", kWriteChunkSize);

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_THAT(log_messages_starting_with(events, "Write flash buffer:"),
                    ElementsAre("Write flash buffer: 0x00000000 (0% - 512000 B/s, ~ 1 s)"));
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, WriteComparesErasesProgramsAndVerifiesFirstChangedBlockWithStaleCrcFrames)
{
    const Case& test_case = kCases[1];
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare_with_stale_crc_frames(transport, blocks_for(test_case), true);
    script_flash_init(transport);
    script_first_flash_block(transport);
    script_compare(transport, blocks_for(test_case), false);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(events.logs, expected_write_logs(blocks_for(test_case)));
    EXPECT_THAT(events.notices, ElementsAre("Writing ROM, please wait..."));
    expect_exact_phase_progress(
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
    auto plan = write_plan(kCases[1], std::move(image));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    RecordingCanTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport.scripted);
    script_sh7058_a5_block_compare(transport.scripted, false);
    script_flash_init(transport.scripted);
    script_sh7058_a5_block_write(transport.scripted);
    script_sh7058_a5_block_compare(transport.scripted, true);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scripted.scriptConsumed());
    EXPECT_THAT(events.notices, ElementsAre("Writing ROM, please wait..."));
    EXPECT_FALSE(has_log(events, LogLevel::Error, "*** ERROR IN FLASH PROCESS ***"));
    EXPECT_TRUE(has_log(events, LogLevel::Debug, "ROM CRC: 0xaaa0b108 IMG CRC: 0xaaa0b108"));

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
        expect_phase(static_cast<std::size_t>(done + 2), "Compare", 2, done, 16);
    }
    expect_phase(19, "Write", 3, 0, 0x18000);
    for (int window = 1; window <= 191; ++window)
    {
        expect_phase(static_cast<std::size_t>(19 + window), "Write", 3, window * 0x200, 0x18000);
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
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, blocks_for(test_case), false);
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    expect_exact_phase_progress(
        events,
        {{"Kernel", 1, 4, 0, 1},    {"Kernel", 1, 4, 1, 1},    {"Compare", 2, 4, 0, 16},  {"Compare", 2, 4, 1, 16},
         {"Compare", 2, 4, 2, 16},  {"Compare", 2, 4, 3, 16},  {"Compare", 2, 4, 4, 16},  {"Compare", 2, 4, 5, 16},
         {"Compare", 2, 4, 6, 16},  {"Compare", 2, 4, 7, 16},  {"Compare", 2, 4, 8, 16},  {"Compare", 2, 4, 9, 16},
         {"Compare", 2, 4, 10, 16}, {"Compare", 2, 4, 11, 16}, {"Compare", 2, 4, 12, 16}, {"Compare", 2, 4, 13, 16},
         {"Compare", 2, 4, 14, 16}, {"Compare", 2, 4, 15, 16}, {"Compare", 2, 4, 16, 16}, {"Write", 3, 4, 0, 0},
         {"Complete", 4, 4, 0, 1},  {"Complete", 4, 4, 1, 1}});
}

TEST(SubaruTcuDensoSh705xCanExecutor, CrcDrainIgnoresMalformedAndWrongIdFramesForEqualAndMismatchResults)
{
    const std::array<bytes::Bytes, 2> stale_frames{
        {bytes::Bytes{0x12, 0x34}, bytes::Bytes{0x00, 0x00, 0x07, 0xE8, 0xDE, 0xAD}}};
    for (const bytes::Bytes& stale : stale_frames)
    {
        for (const bool mismatch : {false, true})
        {
            auto plan = write_plan(kCases[1]);
            ASSERT_TRUE(plan.has_value()) << plan.error().detail;
            SubaruTcuDensoSh705xCanExecutor executor;
            ScriptedCanFlashTransport transport;
            configure_and_open(executor, *plan, transport);
            script_kernel_alive(transport);
            script_crc_with_stale(transport, kSh7058Blocks[0],
                                  mismatch ? std::uint32_t{0xF722EF48} : std::uint32_t{0xF722EF49}, stale);
            for (std::size_t index = 1; index < kSh7058Blocks.size(); ++index)
            {
                script_crc(transport, kSh7058Blocks[index], kSh7058Blocks[index].zero_crc);
            }
            ToggleCancellation cancellation;
            FakeClock clock;
            PhaseCancellingEventSink events(cancellation, mismatch ? "Compare" : "never", 16);

            auto result = executor.execute(*plan, transport, clock, cancellation, events);

            if (mismatch)
            {
                ASSERT_FALSE(result.has_value());
                EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
            }
            else
            {
                ASSERT_TRUE(result.has_value()) << result.error().detail;
            }
            EXPECT_TRUE(transport.scriptConsumed());
            EXPECT_TRUE(has_log(events, LogLevel::Info, mismatch ? "\tNO" : "\tYES"));
        }
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, CrcDrainPropagatesCancellationAndDisconnect)
{
    for (const ErrorKind error_kind : {ErrorKind::Cancelled, ErrorKind::Disconnected})
    {
        auto plan = write_plan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        transport.expectWrite(bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xBE, 0xEF, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x10, 0x00});
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x05, 0x42, 0xF7, 0x22, 0xEF, 0x49});
        transport.queue_error(error_kind, "stale drain interrupted");
        NeverCancelled cancellation;
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, error_kind);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, CancellationInterruptsUploadReadCrcAndWriteLoops)
{
    // Upload loop: cancellation immediately after the first fixed B6 write
    // must prevent Kernel completion and propagate from the pending read.
    {
        bytes::Bytes kernel(129, bytes::Byte{0x00});
        kernel.back() = 0x01;
        auto plan = read_plan(kCases[0], std::move(kernel));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        RecordingCanTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_probe_timeout(transport.scripted);
        script_identity_queries(transport.scripted);
        script_strict_session_and_security(transport.scripted);
        script_hand_derived_129_byte_sh7055_kernel_upload(transport.scripted);
        ToggleCancellation cancellation;
        transport.cancellation_to_trigger = &cancellation;
        transport.cancel_prefix = bytes::Bytes{0x00, 0x00, 0x07, 0xE1, 0xB6};
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_THAT(events.notices, ElementsAre("Preparing, please wait..."));
        expect_exact_phase_progress(events, {{"Kernel", 1, 2, 0, 1}});
    }

    // Read loop: one complete 0x400 page is reported, then the next boundary
    // observes cancellation before sending another request.
    {
        auto plan = read_plan(kCases[0]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_read_pages(transport, kReadPageSize);
        ToggleCancellation cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Read", kReadPageSize);

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Read");
        EXPECT_EQ(events.phase_progress_calls.back().done, kReadPageSize);
    }

    // CRC loop: cancellation on the first completed comparison is observed
    // by the legacy 5 ms pacing checkpoint before block 2.
    {
        auto plan = write_plan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_crc(transport, kSh7058Blocks[0], 0xF722EF49);
        ToggleCancellation cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Compare", 1);

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Compare");
        EXPECT_EQ(events.phase_progress_calls.back().done, 1);
    }

    // Write loop: cancellation after one 0x200 buffer update stops before
    // the second window and retains the post-erase recovery warning.
    {
        auto plan = write_plan(kCases[1]);
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_alive(transport);
        script_compare(transport, blocks_for(kCases[1]), true);
        script_flash_init(transport);
        transport.expectWrite(beef_request(0x04));
        transport.queueRead(beef_response(0x44, bytes::Bytes{0x00, 0x64}));
        transport.expectWrite(beef_request(0x25, composeBe(std::uint32_t{0x00000000})));
        transport.queueRead(beef_response(0x65));
        transport.expectWrite(beef_request(0x22, composeBe(std::uint32_t{0}, zero_chunk(kWriteChunkSize))));
        transport.queueRead(beef_response(0x62));
        ToggleCancellation cancellation;
        FakeClock clock;
        PhaseCancellingEventSink events(cancellation, "Write", kWriteChunkSize);

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_TRUE(transport.scriptConsumed());
        EXPECT_EQ(events.phase_progress_calls.back().phase_name, "Write");
        EXPECT_EQ(events.phase_progress_calls.back().done, kWriteChunkSize);
        EXPECT_TRUE(has_log(events, LogLevel::Error, kReflashRecoveryWarning));
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, RejectsInvalidPlanBeforeTransportInteraction)
{
    auto fields = FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruTcuDensoSh705xCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(kCases[0].protocol),
        .mcu_name = std::string(kCases[0].mcu),
        .transfer_region = {0, kCases[0].rom_size},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = kernel_for(kCases[0]),
        .family_plan = SubaruTcuDensoSh705xCanPlan{.request_id = 0x7E0},
        .confirmations = {},
    };
    auto plan = validate_and_build(std::move(fields));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruTcuDensoSh705xCanExecutor, MalformedTimeoutNegativeAndDisconnectRepliesAreTyped)
{
    auto plan = read_plan(kCases[1]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    NeverCancelled cancellation;

    {
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectWrite(kernel_id_request());
        transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xE9, 0xBE, 0xEF, 0x00, 0x01});
        transport.queue_no_frame();
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    }
    {
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        script_kernel_probe_timeout(transport);
        script_identity_queries(transport);
        transport.expectWrite(request(bytes::Bytes{0x10, 0x03}));
        transport.queueRead(response(bytes::Bytes{0x7F, 0x10, 0x22}));
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    }
    {
        SubaruTcuDensoSh705xCanExecutor executor;
        ScriptedCanFlashTransport transport;
        configure_and_open(executor, *plan, transport);
        transport.expectWrite(kernel_id_request());
        transport.queue_error(ErrorKind::Disconnected, "adapter dropped");
        FakeClock clock;
        RecordingEventSink events;

        auto result = executor.execute(*plan, transport, clock, cancellation, events);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    }
}

TEST(SubaruTcuDensoSh705xCanExecutor, CancellationAfterEraseReturnsRecoveryWarning)
{
    const Case& test_case = kCases[1];
    auto plan = write_plan(test_case);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    SubaruTcuDensoSh705xCanExecutor executor;
    ScriptedCanFlashTransport transport;
    configure_and_open(executor, *plan, transport);
    script_kernel_alive(transport);
    script_compare(transport, blocks_for(test_case), true);
    script_flash_init(transport);
    script_first_flash_block(transport, false);
    ToggleCancellation cancellation;
    FakeClock clock;
    CancellingEventSink events(cancellation);

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(has_log(events, LogLevel::Error, kReflashRecoveryWarning));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuDensoSh705xCanExecutor, BoundAttemptReturnsCloseErrorOnlyWhenExecutionSucceeds)
{
    auto plan = read_plan(kCases[0]);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    auto success_transport = std::make_unique<ScriptedCanFlashTransport>();
    script_kernel_alive(*success_transport);
    script_read_pages(*success_transport, kCases[0].rom_size);
    success_transport->close_result_ = fail(ErrorKind::Disconnected, "close failed");
    auto success_attempt =
        bind_flash_attempt(*plan, std::make_unique<SubaruTcuDensoSh705xCanExecutor>(), std::move(success_transport));
    NeverCancelled cancellation;
    FakeClock clock;
    RecordingEventSink events;

    auto close_only = success_attempt->run(clock, cancellation, events);

    ASSERT_FALSE(close_only.has_value());
    EXPECT_EQ(close_only.error().kind, ErrorKind::Disconnected);

    auto failing_transport = std::make_unique<ScriptedCanFlashTransport>();
    script_kernel_alive(*failing_transport);
    failing_transport->expectWrite(beef_request(0x03, composeBe(0x00_b, u24(0), std::uint16_t{kReadPageSize})));
    failing_transport->queue_no_frame();
    failing_transport->close_result_ = fail(ErrorKind::Internal, "close also failed");
    auto failing_attempt =
        bind_flash_attempt(*plan, std::make_unique<SubaruTcuDensoSh705xCanExecutor>(), std::move(failing_transport));
    RecordingEventSink failing_events;

    auto execution_error = failing_attempt->run(clock, cancellation, failing_events);

    ASSERT_FALSE(execution_error.has_value());
    EXPECT_EQ(execution_error.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(has_log(failing_events, LogLevel::Warning, "close failed after execution error"));
}

} // namespace
} // namespace fastecu::flash
