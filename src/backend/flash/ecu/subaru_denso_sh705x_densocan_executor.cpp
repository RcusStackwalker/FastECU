#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.h"

#include <array>
#include <format>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;

constexpr std::uint32_t kIsoRequestId = 0x7E0;
constexpr std::uint32_t kRawTransmitId = 0x000FFFFE;
constexpr std::uint32_t kRawReceiveId = 0x21;
constexpr std::uint16_t kStartComm = 0xBEEF;
constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::uint32_t kWriteChunkSize = 0x200;
constexpr std::uint32_t kCommitBlockSize = 0x1000;
constexpr int kKernelIdTimeoutMs = 800;
constexpr int kRawTimeoutMs = 800;
constexpr int kPageTimeoutMs = 3000;
constexpr int kCrcTimeoutMs = 3000;
constexpr int kFlashInitTimeoutMs = 500;
constexpr int kFlashTimeoutMs = 3000;

constexpr std::array<std::uint16_t, 4> kEncryptPayloadKeys{0x7856, 0xCE22, 0xF513, 0x6E86};
constexpr std::array<std::uint16_t, 4> kDecryptPayloadKeys{0x6E86, 0xF513, 0xCE22, 0x7856};
constexpr std::array<std::uint8_t, 32> kIndexTransformation{
    0x05, 0x06, 0x07, 0x01, 0x09, 0x0C, 0x0D, 0x08, 0x0A, 0x0D, 0x02, 0x0B, 0x0F, 0x04, 0x00, 0x03,
    0x0B, 0x04, 0x06, 0x00, 0x0F, 0x02, 0x0D, 0x09, 0x05, 0x0C, 0x01, 0x0A, 0x03, 0x0D, 0x0E, 0x08,
};

Status check_cancelled(const ICancellationToken& cancellation, std::string detail)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::move(detail));
    }
    return {};
}

bytes::Bytes iso_request(std::uint8_t opcode, bytes::ByteView payload = {})
{
    bytes::Bytes request = composeBe(kIsoRequestId, kStartComm, static_cast<std::uint16_t>(payload.size() + 1),
                                     static_cast<bytes::Byte>(opcode));
    request.insert(request.end(), payload.begin(), payload.end());
    return request;
}

// Legacy request_kernel_id(),
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.cpp:1435-1484.
// The message declares a one-byte BEEF payload but is passed to the legacy
// ISO-15765 driver as a fixed twelve-byte CAN wire buffer with three zeros.
bytes::Bytes kernel_id_request()
{
    return {0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
}

Status iso_write(IMixedCanFlashTransport& transport, bytes::ByteView request, const ICancellationToken& cancellation,
                 std::string_view subject)
{
    if (Status cancelled = check_cancelled(cancellation, std::format("cancelled before {} write", subject)); !cancelled)
    {
        return cancelled;
    }
    if (Status written = transport.write_iso15765(request, cancellation); !written)
    {
        return written;
    }
    return check_cancelled(cancellation, std::format("cancelled after {} write", subject));
}

Result<std::optional<bytes::Bytes>> iso_read(IMixedCanFlashTransport& transport, int timeout_ms,
                                             const ICancellationToken& cancellation, std::string_view subject)
{
    if (Status cancelled = check_cancelled(cancellation, std::format("cancelled before {} read", subject)); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }
    Result<std::optional<bytes::Bytes>> received = transport.read_iso15765(timeout_ms, cancellation);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = check_cancelled(cancellation, std::format("cancelled after {} read", subject)); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }
    return received;
}

Status raw_write(IMixedCanFlashTransport& transport, cdbg::CanFrame frame, const ICancellationToken& cancellation,
                 std::string_view subject)
{
    if (Status cancelled = check_cancelled(cancellation, std::format("cancelled before {} write", subject)); !cancelled)
    {
        return cancelled;
    }
    if (Status written = transport.write_raw(frame, cancellation); !written)
    {
        return written;
    }
    return check_cancelled(cancellation, std::format("cancelled after {} write", subject));
}

Result<std::optional<cdbg::CanFrame>> raw_read(IMixedCanFlashTransport& transport, int timeout_ms,
                                               const ICancellationToken& cancellation, std::string_view subject)
{
    if (Status cancelled = check_cancelled(cancellation, std::format("cancelled before {} read", subject)); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }
    Result<std::optional<cdbg::CanFrame>> received = transport.read_raw(timeout_ms, cancellation);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = check_cancelled(cancellation, std::format("cancelled after {} read", subject)); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }
    return received;
}

Status expect_iso_response(bytes::ByteView response, std::uint8_t expected_opcode, std::size_t min_payload,
                           std::string_view subject)
{
    // DensoCAN BEEF is a proprietary kernel envelope, not a UDS PDU. Its
    // fixed transport identifier occupies 0..3; BEEF, declared length and
    // opcode then occupy 4..8. Legacy checks the BEEF/opcode bytes at
    // 512-657, 820-905 and 906-1395; the guards below also make each later
    // indexed byte safe.
    if (response.size() < 9)
    {
        return fail(ErrorKind::BadResponse, std::format("{} response is shorter than the BEEF envelope", subject));
    }
    if (response[0] != 0x00 || response[1] != 0x00 || response[2] != 0x07 || response[3] != 0xE8 ||
        response[4] != 0xBE || response[5] != 0xEF || response[8] != expected_opcode)
    {
        return fail(ErrorKind::BadResponse, std::format("Wrong response from ECU during {}", subject));
    }
    const std::size_t declared_payload = bytes::readU16Be(response, 6);
    if (declared_payload == 0 || declared_payload > response.size() - 8 || declared_payload - 1 < min_payload ||
        response.size() - 9 < min_payload)
    {
        return fail(ErrorKind::BadResponse, std::format("Truncated response from ECU during {}", subject));
    }
    return {};
}

Status expect_raw_response(const cdbg::CanFrame& response, std::uint8_t first, std::uint8_t second,
                           std::string_view subject)
{
    if (response.id != kRawReceiveId)
    {
        return fail(ErrorKind::BadResponse, std::format("Wrong raw CAN response ID during {}", subject));
    }
    if (response.payload.size() < 2)
    {
        return fail(ErrorKind::BadResponse, std::format("Truncated raw CAN response during {}", subject));
    }
    if (response.payload[0] != first || response.payload[1] != second)
    {
        return fail(ErrorKind::BadResponse, std::format("Wrong raw CAN response during {}", subject));
    }
    return {};
}

Result<bool> probe_kernel(IMixedCanFlashTransport& transport, const ICancellationToken& cancellation,
                          IEventSink& events)
{
    // Legacy connect_bootloader(), lines 106-153. A bounded absent response
    // is the only normal fall-through into raw bootloader mode; a malformed
    // or disconnected response fails closed instead of switching hardware
    // modes on an uncertain state.
    events.log(LogLevel::Info, "Checking if kernel is already running...");
    events.log(LogLevel::Info, "Requesting kernel ID");
    const bytes::Bytes request = kernel_id_request();
    if (Status written = iso_write(transport, request, cancellation, "kernel ID"); !written)
    {
        return std::unexpected(written.error());
    }
    Result<std::optional<bytes::Bytes>> received = iso_read(transport, kKernelIdTimeoutMs, cancellation, "kernel ID");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        events.log(LogLevel::Info, "No response from kernel, continuing with bootloader initialization");
        return false;
    }
    if (Status valid = expect_iso_response(**received, 0x41, 0, "kernel ID"); !valid)
    {
        return std::unexpected(valid.error());
    }
    events.log(LogLevel::Info, "Kernel already running");
    return true;
}

Status require_kernel_id(IMixedCanFlashTransport& transport, const ICancellationToken& cancellation, IEventSink& events)
{
    // Legacy upload_kernel(), lines 475-507, repeats the same proprietary
    // BEEF exchange only after the raw bootloader jump and ISO transition.
    events.log(LogLevel::Info, "Requesting kernel ID");
    const bytes::Bytes request = kernel_id_request();
    if (Status written = iso_write(transport, request, cancellation, "kernel ID"); !written)
    {
        return written;
    }
    Result<std::optional<bytes::Bytes>> received = iso_read(transport, kKernelIdTimeoutMs, cancellation, "kernel ID");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU after kernel upload");
    }
    if (Status valid = expect_iso_response(**received, 0x41, 0, "kernel ID"); !valid)
    {
        return valid;
    }
    events.log(LogLevel::Info, "Kernel is alive");
    return {};
}

Status wake_bootloader(IMixedCanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                       IEventSink& events)
{
    // Legacy connect_bootloader(), lines 157-222. The portable executor
    // retains the exact 1,000 raw wake frames, adding a cooperative check at
    // every iteration so this bounded loop cannot become an uncancellable UI
    // stall.
    const cdbg::CanFrame wake{.id = kRawTransmitId, .payload = {0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    events.log(LogLevel::Info, "Initializing bootloader");
    for (int count = 0; count < 1000; ++count)
    {
        if (Status cancelled = check_cancelled(cancellation, "cancelled during DensoCAN wake"); !cancelled)
        {
            return cancelled;
        }
        if (Status written = raw_write(transport, wake, cancellation, "DensoCAN wake"); !written)
        {
            return written;
        }
        if (Status slept = clock.sleep(3, cancellation); !slept)
        {
            return slept;
        }
        events.progress(count + 1, 1000);
    }
    if (Status cleared = transport.clear_receive_buffer(); !cleared)
    {
        return cleared;
    }

    const cdbg::CanFrame check{.id = kRawTransmitId, .payload = {0x7A, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    if (Status written = raw_write(transport, check, cancellation, "bootloader handshake"); !written)
    {
        return written;
    }
    Result<std::optional<cdbg::CanFrame>> received =
        raw_read(transport, kRawTimeoutMs, cancellation, "bootloader handshake");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from DensoCAN bootloader");
    }
    if (Status valid = expect_raw_response(**received, 0x7A, 0x96, "bootloader handshake"); !valid)
    {
        return valid;
    }
    events.log(LogLevel::Debug, "Connected to bootloader");
    return {};
}

cdbg::CanFrame raw_address_command(std::uint8_t command, std::uint32_t address)
{
    return {.id = kRawTransmitId,
            .payload = {0x7A, command, static_cast<bytes::Byte>(address >> 24), static_cast<bytes::Byte>(address >> 16),
                        static_cast<bytes::Byte>(address >> 8), static_cast<bytes::Byte>(address), 0x00, 0x00}};
}

Status set_raw_kernel_address(IMixedCanFlashTransport& transport, std::uint32_t address,
                              const ICancellationToken& cancellation, std::string_view subject)
{
    if (Status written = raw_write(transport, raw_address_command(0x9C, address), cancellation, subject); !written)
    {
        return written;
    }
    Result<std::optional<cdbg::CanFrame>> received = raw_read(transport, kRawTimeoutMs, cancellation, subject);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, std::format("no response from ECU during {}", subject));
    }
    return expect_raw_response(**received, 0x7A, 0x9C, subject);
}

Status upload_kernel(IMixedCanFlashTransport& transport, const KernelImage& kernel, IClock& clock,
                     const ICancellationToken& cancellation, IEventSink& events)
{
    // Legacy upload_kernel(), lines 227-507. This raw Denso bootloader path
    // uses a 29-bit transmit ID, six payload bytes per 0x7A/0xAE block, and
    // a zero-padded final block. BEEF is not used in this mode.
    if (kernel.bytes.empty())
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN kernel image is empty");
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before kernel upload"); !cancelled)
    {
        return cancelled;
    }
    bytes::Bytes padded = kernel.bytes;
    padded.resize((padded.size() + 5U) / 6U * 6U, 0);
    if (padded.empty() || padded.size() > std::numeric_limits<std::uint32_t>::max() - kernel.load_address - 1U)
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN padded kernel range overflows");
    }

    events.log(LogLevel::Info, "Set kernel upload address");
    if (Status addressed =
            set_raw_kernel_address(transport, kernel.load_address, cancellation, "kernel upload address");
        !addressed)
    {
        return addressed;
    }

    events.log(LogLevel::Info, "Uploading kernel, please wait...");
    for (std::size_t offset = 0; offset < padded.size(); offset += 6)
    {
        if (Status cancelled = check_cancelled(cancellation, "cancelled during kernel upload"); !cancelled)
        {
            return cancelled;
        }
        cdbg::CanFrame block{.id = kRawTransmitId, .payload = {0x7A, 0xAE}};
        block.payload.insert(block.payload.end(), padded.begin() + static_cast<std::ptrdiff_t>(offset),
                             padded.begin() + static_cast<std::ptrdiff_t>(offset + 6));
        if (Status written = raw_write(transport, std::move(block), cancellation, "kernel upload block"); !written)
        {
            return written;
        }
        if (Status slept = clock.sleep(1, cancellation); !slept)
        {
            return slept;
        }
        events.progress(static_cast<int>(offset + 6), static_cast<int>(padded.size()));
    }

    const std::uint32_t end_plus_one = kernel.load_address + static_cast<std::uint32_t>(padded.size()) + 1U;
    if (Status written = raw_write(transport, raw_address_command(0xB4, end_plus_one), cancellation, "kernel checksum");
        !written)
    {
        return written;
    }
    if (Status slept = clock.sleep(200, cancellation); !slept)
    {
        return slept;
    }
    Result<std::optional<cdbg::CanFrame>> checksum =
        raw_read(transport, kRawTimeoutMs, cancellation, "kernel checksum");
    if (!checksum)
    {
        return std::unexpected(checksum.error());
    }
    if (!checksum->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU during kernel checksum");
    }
    if (Status valid = expect_raw_response(**checksum, 0x7A, 0xB1, "kernel checksum"); !valid)
    {
        return valid;
    }

    if (Status addressed = set_raw_kernel_address(transport, kernel.load_address, cancellation, "kernel jump address");
        !addressed)
    {
        return addressed;
    }
    const cdbg::CanFrame jump{.id = kRawTransmitId, .payload = {0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    if (Status written = raw_write(transport, jump, cancellation, "kernel jump"); !written)
    {
        return written;
    }
    if (Status slept = clock.sleep(200, cancellation); !slept)
    {
        return slept;
    }
    Result<std::optional<cdbg::CanFrame>> jumped = raw_read(transport, 50, cancellation, "kernel jump");
    if (!jumped)
    {
        return std::unexpected(jumped.error());
    }
    // Legacy lines 446-463 log but deliberately do not return on a missing
    // or wrong 7A/A0 response. Preserve that tolerant jump acknowledgement.
    if (jumped->has_value())
    {
        if (Status jump_response = expect_raw_response(**jumped, 0x7A, 0xA0, "kernel jump"); !jump_response)
        {
            events.log(LogLevel::Warning, "Unexpected kernel-jump response; continuing to ISO kernel probe");
        }
    }
    return {};
}

bytes::Bytes decrypt_payload(bytes::ByteView wire)
{
    return SsmProtocol::calculatePayload(wire, static_cast<std::uint32_t>(wire.size()), kDecryptPayloadKeys,
                                         kIndexTransformation);
}

bytes::Bytes encrypt_payload(bytes::ByteView plain)
{
    return SsmProtocol::calculatePayload(plain, static_cast<std::uint32_t>(plain.size()), kEncryptPayloadKeys,
                                         kIndexTransformation);
}

Result<bytes::Bytes> read_mem(IMixedCanFlashTransport& transport, const MemoryRegion& region, IClock&,
                              const ICancellationToken& cancellation, IEventSink& events, PhaseReporter& phase)
{
    // Legacy read_mem(), lines 512-657. The original appends raw payloads;
    // its companion decrypt_payload() at 1415-1429 establishes that a
    // portable ROM result is the decrypted page sequence. Every 0x400-byte
    // response must now contain its entire page before it is indexed.
    bytes::Bytes rom;
    rom.reserve(region.length);
    events.log(LogLevel::Info, "Start reading ROM, please wait...");
    for (std::uint32_t offset = 0; offset < region.length; offset += kReadPageSize)
    {
        if (Status cancelled = check_cancelled(cancellation, "cancelled during ROM read"); !cancelled)
        {
            return std::unexpected(cancelled.error());
        }
        const std::uint32_t address = region.start + offset;
        const bytes::Bytes request = iso_request(0x03, composeBe(0x00_b, u24(address), std::uint16_t{kReadPageSize}));
        if (Status written = iso_write(transport, request, cancellation, "ROM read"); !written)
        {
            return std::unexpected(written.error());
        }
        Result<std::optional<bytes::Bytes>> received = iso_read(transport, kPageTimeoutMs, cancellation, "ROM read");
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            return fail(ErrorKind::Timeout, "no response from ECU during ROM read");
        }
        if (Status valid = expect_iso_response(**received, 0x43, kReadPageSize, "ROM read"); !valid)
        {
            return std::unexpected(valid.error());
        }
        const bytes::ByteView wire_page(**received);
        const bytes::Bytes plain_page = decrypt_payload(wire_page.subspan(9, kReadPageSize));
        if (plain_page.size() != kReadPageSize)
        {
            return fail(ErrorKind::BadResponse, "DensoCAN decrypted page has an invalid length");
        }
        rom.insert(rom.end(), plain_page.begin(), plain_page.end());
        const int done = static_cast<int>(offset + kReadPageSize);
        events.progress(done, static_cast<int>(region.length));
        phase.update(done);
    }
    events.log(LogLevel::Info, "ROM read ready");
    phase.complete();
    return rom;
}

Result<std::uint32_t> read_block_crc(IMixedCanFlashTransport& transport, const MemoryRegion& block,
                                     const ICancellationToken& cancellation)
{
    // Legacy check_romcrc(), lines 820-905. The seven-byte CRC request body
    // is [address:4][00][length:3]; response bytes 9..12 hold the ECU CRC.
    const bytes::Bytes request = iso_request(0x02, composeBe(block.start, 0x00_b, u24(block.length)));
    if (Status written = iso_write(transport, request, cancellation, "ROM CRC"); !written)
    {
        return std::unexpected(written.error());
    }
    Result<std::optional<bytes::Bytes>> received = iso_read(transport, kCrcTimeoutMs, cancellation, "ROM CRC");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU during ROM CRC");
    }
    if (Status valid = expect_iso_response(**received, 0x42, 4, "ROM CRC"); !valid)
    {
        return std::unexpected(valid.error());
    }
    const std::uint32_t crc = bytes::readU32Be(**received, 9);
    // Legacy lines 883 and 904 intentionally perform this short stale-byte
    // drain after both equal and unequal CRC outcomes.
    Result<std::optional<bytes::Bytes>> drained = iso_read(transport, 200, cancellation, "ROM CRC drain");
    if (!drained)
    {
        return std::unexpected(drained.error());
    }
    return crc;
}

Status set_flash_mode(IMixedCanFlashTransport& transport, bool enabled, const ICancellationToken& cancellation)
{
    const std::uint8_t command = enabled ? 0x20 : 0x21;
    if (Status written = iso_write(transport, iso_request(command), cancellation, "flash mode"); !written)
    {
        return written;
    }
    Result<std::optional<bytes::Bytes>> received = iso_read(transport, kFlashInitTimeoutMs, cancellation, "flash mode");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU during flash mode setup");
    }
    return expect_iso_response(**received, static_cast<std::uint8_t>(command | 0x40U), 0, "flash mode");
}

Status init_flash_write(IMixedCanFlashTransport& transport, bool test_write, const ICancellationToken& cancellation,
                        IEventSink& events)
{
    // Legacy init_flash_write(), lines 906-1059. Returned max lengths are
    // retained as diagnostic evidence; the legacy flash_block() explicitly
    // selects 0x1000 commit windows regardless of the reported block size.
    for (const std::uint8_t command : {std::uint8_t{0x05}, std::uint8_t{0x06}})
    {
        if (Status written = iso_write(transport, iso_request(command), cancellation, "flash initialization"); !written)
        {
            return written;
        }
        Result<std::optional<bytes::Bytes>> received =
            iso_read(transport, kFlashInitTimeoutMs, cancellation, "flash initialization");
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            return fail(ErrorKind::Timeout, "no response from ECU during flash initialization");
        }
        if (Status valid =
                expect_iso_response(**received, static_cast<std::uint8_t>(command | 0x40U), 4, "flash initialization");
            !valid)
        {
            return valid;
        }
        const std::uint32_t value = bytes::readU32Be(**received, 9);
        events.log(LogLevel::Info,
                   std::format("{}: 0x{:08X}", command == 0x05 ? "Max message length" : "Flash block size", value));
    }
    // Legacy test write sends FLASH_DISABLE instead of FLASH_ENABLE at
    // lines 1014-1019, then validates rather than commits each 0x1000 block.
    return set_flash_mode(transport, !test_write, cancellation);
}

Status query_programming_voltage(IMixedCanFlashTransport& transport, const ICancellationToken& cancellation,
                                 IEventSink& events)
{
    // Legacy reflash_block(), lines 1087-1150. Require bytes 9 and 10 before
    // decoding the voltage; legacy's >7 check did not make that access safe.
    if (Status written = iso_write(transport, iso_request(0x04), cancellation, "programming voltage"); !written)
    {
        return written;
    }
    Result<std::optional<bytes::Bytes>> received =
        iso_read(transport, kFlashInitTimeoutMs, cancellation, "programming voltage");
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU during programming-voltage query");
    }
    if (Status valid = expect_iso_response(**received, 0x44, 2, "programming voltage"); !valid)
    {
        return valid;
    }
    const std::uint16_t voltage_raw =
        (static_cast<std::uint16_t>((**received)[9]) << 8U) | static_cast<std::uint16_t>((**received)[10]);
    const float voltage = static_cast<float>(voltage_raw) / 50.0F;
    events.log(LogLevel::Info, std::format("Programming voltage: {:.2f}V", voltage));
    return {};
}

Status flash_block(IMixedCanFlashTransport& transport, bytes::ByteView wire_image, const MemoryRegion& block,
                   bool test_write, const ICancellationToken& cancellation, IEventSink& events, PhaseReporter& phase)
{
    // Legacy flash_block(), lines 1153-1395. Each physical block is blanked
    // only for a real write, transferred in 0x200 chunks, and committed (or
    // validated for test-write) in fixed 0x1000 windows.
    if (block.start > wire_image.size() || block.length > wire_image.size() - block.start ||
        block.length % kWriteChunkSize != 0 || block.length % kCommitBlockSize != 0)
    {
        return fail(ErrorKind::InvalidConfig, "flash block is not represented by the encrypted ROM image");
    }
    if (!test_write)
    {
        events.log(LogLevel::Info, "Erasing flash page...");
        if (Status written =
                iso_write(transport, iso_request(0x25, composeBe(block.start)), cancellation, "flash erase");
            !written)
        {
            return written;
        }
        Result<std::optional<bytes::Bytes>> erased = iso_read(transport, kFlashTimeoutMs, cancellation, "flash erase");
        if (!erased)
        {
            return std::unexpected(erased.error());
        }
        if (!erased->has_value())
        {
            return fail(ErrorKind::Timeout, "no response from ECU during flash erase");
        }
        if (Status valid = expect_iso_response(**erased, 0x65, 0, "flash erase"); !valid)
        {
            return valid;
        }
    }

    for (std::uint32_t offset = 0; offset < block.length; offset += kWriteChunkSize)
    {
        if (Status cancelled = check_cancelled(cancellation, "cancelled during flash buffer transfer"); !cancelled)
        {
            return cancelled;
        }
        const std::uint32_t address = block.start + offset;
        const bytes::Bytes request =
            iso_request(0x22, composeBe(address, wire_image.subspan(address, kWriteChunkSize)));
        if (Status written = iso_write(transport, request, cancellation, "flash buffer transfer"); !written)
        {
            return written;
        }
        Result<std::optional<bytes::Bytes>> received =
            iso_read(transport, kFlashInitTimeoutMs, cancellation, "flash buffer transfer");
        if (!received)
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            return fail(ErrorKind::Timeout, "no response from ECU during flash buffer transfer");
        }
        if (Status valid = expect_iso_response(**received, 0x62, 0, "flash buffer transfer"); !valid)
        {
            return valid;
        }
        const int done = static_cast<int>(offset + kWriteChunkSize);
        events.progress(done, static_cast<int>(block.length));
        phase.update(done);

        if ((offset + kWriteChunkSize) % kCommitBlockSize == 0)
        {
            const std::uint32_t commit_start = address + kWriteChunkSize - kCommitBlockSize;
            const std::uint32_t crc = checksum::crc32(wire_image.subspan(commit_start, kCommitBlockSize));
            const std::uint8_t command = test_write ? 0x23 : 0x24;
            const bytes::Bytes commit =
                iso_request(command, composeBe(commit_start, std::uint16_t{kCommitBlockSize}, crc));
            if (Status written = iso_write(transport, commit, cancellation, "flash commit"); !written)
            {
                return written;
            }
            Result<std::optional<bytes::Bytes>> committed =
                iso_read(transport, kFlashTimeoutMs, cancellation, "flash commit");
            if (!committed)
            {
                return std::unexpected(committed.error());
            }
            if (!committed->has_value())
            {
                return fail(ErrorKind::Timeout, "no response from ECU during flash commit");
            }
            if (Status valid =
                    expect_iso_response(**committed, static_cast<std::uint8_t>(command | 0x40U), 0, "flash commit");
                !valid)
            {
                return valid;
            }
        }
    }
    return {};
}

Status write_mem(IMixedCanFlashTransport& transport, const FlashPlan& plan, IClock&,
                 const ICancellationToken& cancellation, IEventSink& events, PhaseReporter& phase)
{
    // Legacy write_mem()/get_changed_blocks(), lines 662-819, then
    // check_romcrc()/init_flash_write()/reflash_block()/flash_block(),
    // lines 820-1395. The portable plan retains every one of the sixteen
    // physical regions, so the image is addressed by its absolute flash
    // offsets exactly as legacy's data_array was.
    if (!plan.image().has_value())
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN write requires a ROM image");
    }
    const flashdev_t *device = find_flash_device(plan.mcu_name());
    if (device == nullptr || device->numblocks != plan.erase_regions().size())
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN flash geometry is unavailable");
    }
    if (plan.image()->size() > std::numeric_limits<std::uint32_t>::max())
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN ROM image is too large for payload encryption");
    }
    const bytes::Bytes wire_image = encrypt_payload(*plan.image());
    if (wire_image.size() != plan.image()->size())
    {
        return fail(ErrorKind::InvalidConfig, "DensoCAN ROM image is not aligned for payload encryption");
    }
    const bool test_write = plan.operation() == FlashOperation::TestWrite;
    std::vector<bool> modified(device->numblocks, false);
    const auto compare_blocks = [&](bool record_progress) -> Result<unsigned>
    {
        unsigned count = 0;
        for (unsigned index = 0; index < device->numblocks; ++index)
        {
            if (Status cancelled = check_cancelled(cancellation, "cancelled during ROM comparison"); !cancelled)
            {
                return std::unexpected(cancelled.error());
            }
            const MemoryRegion block{device->fblocks[index].start, device->fblocks[index].len};
            Result<std::uint32_t> ecu_crc = read_block_crc(transport, block, cancellation);
            if (!ecu_crc)
            {
                return std::unexpected(ecu_crc.error());
            }
            const std::uint32_t image_crc =
                checksum::crc32(bytes::ByteView(wire_image).subspan(block.start, block.length));
            modified[index] = *ecu_crc != image_crc;
            count += modified[index] ? 1U : 0U;
            if (record_progress)
            {
                phase.update(static_cast<int>(index + 1));
            }
        }
        return count;
    };

    events.log(LogLevel::Info, "Comparing ECU flash memory pages to image file");
    Result<unsigned> changed_count = compare_blocks(true);
    if (!changed_count)
    {
        return std::unexpected(changed_count.error());
    }
    if (*changed_count == 0)
    {
        events.log(LogLevel::Info, "No difference between ROM and ECU data, no flashing needed");
        phase.complete();
        return {};
    }
    if (Status initialized = init_flash_write(transport, test_write, cancellation, events); !initialized)
    {
        return initialized;
    }

    for (unsigned index = 0; index < device->numblocks; ++index)
    {
        if (!modified[index])
        {
            continue;
        }
        const MemoryRegion block{device->fblocks[index].start, device->fblocks[index].len};
        if (Status voltage = query_programming_voltage(transport, cancellation, events); !voltage)
        {
            return voltage;
        }
        if (Status flashed = flash_block(transport, wire_image, block, test_write, cancellation, events, phase);
            !flashed)
        {
            return flashed;
        }
        events.log(LogLevel::Info, std::format("Block {} reflash complete", index));
    }

    events.log(LogLevel::Info, "Comparing ECU flash memory pages to image file after reflash");
    Result<unsigned> remaining = compare_blocks(false);
    if (!remaining)
    {
        return std::unexpected(remaining.error());
    }
    if (test_write)
    {
        events.log(LogLevel::Info, "Test write PASS, it is safe to perform the actual write");
    }
    else
    {
        // The legacy code leaves the write-enable state implied by the kernel
        // after a successful write. The task's portable safety contract makes
        // the final protect/disable exchange explicit before returning.
        if (Status disabled = set_flash_mode(transport, false, cancellation); !disabled)
        {
            return disabled;
        }
        if (*remaining != 0)
        {
            events.log(LogLevel::Error, "Flash verification differs; do not power off, the kernel is still running");
        }
    }
    phase.complete();
    return {};
}

} // namespace

Result<MixedCanConfig> SubaruDensoSh705xDensoCanExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status family = check_family(plan, FlashFamily::SubaruDensoSh705xDensoCan); !family)
    {
        return std::unexpected(family.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_densocan_plan(plan); !valid)
    {
        return std::unexpected(valid.error());
    }
    const auto& wire = std::get<SubaruDensoSh705xDensoCanPlan>(plan.family_plan());
    return MixedCanConfig{
        .kernel = {.bitrate = wire.bitrate,
                   .request_id = wire.iso_request_id,
                   .response_id = wire.iso_response_id,
                   .extended_id = wire.iso_extended_id},
        .bootloader = {.bitrate = wire.bitrate,
                       .transmit_id = wire.raw_transmit_id,
                       .receive_id = wire.raw_receive_id,
                       .extended_id = wire.raw_extended_id},
    };
}

Status SubaruDensoSh705xDensoCanExecutor::before_transport_open(const ICancellationToken& cancellation) const
{
    if (Status cancelled = check_cancelled(cancellation, "cancelled after mixed CAN configuration"); !cancelled)
    {
        return cancelled;
    }
    return check_cancelled(cancellation, "cancelled before opening mixed CAN transport");
}

Result<FlashExecutionResult>
SubaruDensoSh705xDensoCanExecutor::execute(const FlashPlan& plan, IMixedCanFlashTransport& transport, IClock& clock,
                                           const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status family = check_family(plan, FlashFamily::SubaruDensoSh705xDensoCan); !family)
    {
        return std::unexpected(family.error());
    }
    if (Status valid = validate_subaru_denso_sh705x_densocan_plan(plan); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (Status cancelled = check_cancelled(cancellation, "cancelled before DensoCAN execution"); !cancelled)
    {
        return std::unexpected(cancelled.error());
    }

    PhaseSequence phases(events, plan.operation() == FlashOperation::Read ? 2 : 3);
    auto kernel_phase = phases.start("Kernel", 1);
    Result<bool> kernel_alive = probe_kernel(transport, cancellation, events);
    if (!kernel_alive)
    {
        return std::unexpected(kernel_alive.error());
    }
    if (!*kernel_alive)
    {
        if (Status raw_mode = transport.enter_raw_bootloader_mode(); !raw_mode)
        {
            return std::unexpected(raw_mode.error());
        }
        if (Status woken = wake_bootloader(transport, clock, cancellation, events); !woken)
        {
            return std::unexpected(woken.error());
        }
        if (!plan.kernel().has_value())
        {
            return fail(ErrorKind::InvalidConfig, "DensoCAN plan lacks the required kernel image");
        }
        if (Status uploaded = upload_kernel(transport, *plan.kernel(), clock, cancellation, events); !uploaded)
        {
            return std::unexpected(uploaded.error());
        }
        if (Status iso_mode = transport.enter_iso15765_kernel_mode(); !iso_mode)
        {
            return std::unexpected(iso_mode.error());
        }
        if (Status kernel_id = require_kernel_id(transport, cancellation, events); !kernel_id)
        {
            return std::unexpected(kernel_id.error());
        }
    }
    kernel_phase.complete();

    if (plan.operation() == FlashOperation::Read)
    {
        auto read_phase = phases.start("Read", static_cast<int>(plan.transfer_region().length));
        Result<bytes::Bytes> read =
            read_mem(transport, plan.transfer_region(), clock, cancellation, events, read_phase);
        if (!read)
        {
            return std::unexpected(read.error());
        }
        return FlashExecutionResult{
            .operation = plan.operation(), .read_bytes = std::move(*read), .rom_id = std::nullopt};
    }

    auto compare_phase = phases.start("Compare", 16);
    if (Status written = write_mem(transport, plan, clock, cancellation, events, compare_phase); !written)
    {
        return std::unexpected(written.error());
    }
    auto complete_phase = phases.start("Complete", 1);
    complete_phase.complete();
    return FlashExecutionResult{.operation = plan.operation(), .read_bytes = std::nullopt, .rom_id = std::nullopt};
}

} // namespace fastecu::flash
