#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every protocol exchange below is transcribed from
// src/platform/desktop/common/flash/legacy/tcu/
// flash_tcu_subaru_denso_sh705x_can_operation.cpp at revision 59f4e442:
// connect_bootloader (93-364), upload_kernel (369-627), read_mem (632-779),
// write_mem (784-900), CRC/init/reflash/flash (905-1517), seed/crypto
// (1522-1577), and request_kernel_id (1583-1647).
//
// ISO-15765 is only the envelope transport here. Strict UDS exchanges use
// uds::UdsClient; the tolerant 0xAA/0x09 identity queries and all BEEF
// kernel commands are sent through the channel directly because they are
// vendor/proprietary messages rather than UDS application PDUs.

namespace fastecu::flash
{
namespace
{

using bytes::composeBe;
using namespace bytes::literals;

constexpr int kProbeDelayMs = 100; // request_kernel_id(), lines 1608-1609
constexpr int kExtraShortTimeoutMs = 50;
constexpr int kShortTimeoutMs = 200;
constexpr int kMediumTimeoutMs = 500;
constexpr int kLongTimeoutMs = 800;
constexpr int kReadTimeoutMs = 2000;
constexpr int kExtraLongTimeoutMs = 3000;
constexpr int kPostUploadDelayMs = 500; // upload_kernel(), lines 595-595

constexpr int kKernelPageSize = 0x400;
constexpr int kKernelUploadBlockSize = 128;
constexpr int kFlashBufferSize = 0x200;
constexpr int kFlashCommitSize = 0x1000;
constexpr std::uint32_t kKernelStartComm = 0xBEEF;

constexpr bytes::Byte kKernelId = 0x41;
constexpr bytes::Byte kKernelCrc = 0x02;
constexpr bytes::Byte kKernelReadArea = 0x03;
constexpr bytes::Byte kKernelProgVolt = 0x04;
constexpr bytes::Byte kKernelGetMaxMessage = 0x05;
constexpr bytes::Byte kKernelGetMaxBlock = 0x06;
constexpr bytes::Byte kKernelFlashEnable = 0x20;
constexpr bytes::Byte kKernelWriteBuffer = 0x22;
constexpr bytes::Byte kKernelCommitBuffer = 0x24;
constexpr bytes::Byte kKernelBlankPage = 0x25;

constexpr std::array<std::uint16_t, 16> kSeedKeyTable{0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                                      0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0xC85B, 0x32C0, 0xE282, 0x92A0};
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x92A0, 0xE282, 0x32C0, 0xC85B};
constexpr std::array<std::uint8_t, 32> kIndexTransformation{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                            0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                            0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

constexpr uds::ExchangePolicy kStrictPolicy{
    .pre_read_delay_ms = kExtraShortTimeoutMs,
    .read_timeout_ms = kReadTimeoutMs,
    .pending_timeout_ms = kExtraLongTimeoutMs,
    .max_pending_repeats = 10,
};

struct Context
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    uds::UdsClient& uds;
    uds::IUdsChannel& channel;
};

void info(Context& context, std::string_view message)
{
    context.events.log(LogLevel::Info, message);
}

void debug(Context& context, std::string_view message)
{
    context.events.log(LogLevel::Debug, message);
}

void error(Context& context, std::string_view message)
{
    context.events.log(LogLevel::Error, message);
}

Status cancelled_if_requested(const Context& context, std::string_view detail)
{
    if (context.cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::string(detail));
    }
    return {};
}

bytes::Bytes seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, kIndexTransformation);
}

bytes::Bytes encrypt_payload(bytes::ByteView payload)
{
    return SsmProtocol::calculatePayload(payload, static_cast<std::uint32_t>(payload.size()), kEncryptTable,
                                         kIndexTransformation);
}

bytes::Bytes decrypt_payload(bytes::ByteView payload)
{
    return SsmProtocol::calculatePayload(payload, static_cast<std::uint32_t>(payload.size()), kDecryptTable,
                                         kIndexTransformation);
}

struct BeefMessage
{
    bytes::Byte opcode;
    bytes::ByteView payload;
};

Result<BeefMessage> parse_beef(bytes::ByteView pdu)
{
    if (pdu.size() < 5)
    {
        return fail(ErrorKind::BadResponse, "short BEEF response envelope");
    }
    if (bytes::readU16Be(pdu) != kKernelStartComm)
    {
        return fail(ErrorKind::BadResponse, "wrong BEEF response marker");
    }
    const std::uint16_t declared = bytes::readU16Be(pdu, 2);
    // The legacy serial reader can return a complete ISO payload with
    // transport padding or a coalesced trailing frame.  Its checks only use
    // the declared BEEF body, so retain that tolerance while bounding every
    // indexed field.  The view exposed to callers is limited to the declared
    // opcode-plus-payload body.
    if (declared < 1 || static_cast<std::size_t>(declared) + 4 > pdu.size())
    {
        return fail(ErrorKind::BadResponse, "invalid BEEF response length");
    }
    return BeefMessage{.opcode = pdu[4], .payload = pdu.subspan(5, declared - 1)};
}

bytes::Bytes beef_request(bytes::Byte opcode, bytes::ByteView payload = {})
{
    return composeBe(std::uint16_t{kKernelStartComm}, static_cast<std::uint16_t>(payload.size() + 1), opcode, payload);
}

Result<bytes::Bytes> channel_request(Context& context, bytes::ByteView pdu, int timeout_ms, int delay_ms = 0)
{
    if (const Status checkpoint = cancelled_if_requested(context, "cancelled before CAN request");
        !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    if (const Status sent = context.channel.send(pdu, context.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    if (delay_ms > 0)
    {
        if (const Status slept = context.clock.sleep(delay_ms, context.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    Result<std::optional<bytes::Bytes>> received = context.channel.receive(timeout_ms, context.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no CAN response within the read timeout");
    }
    return std::move(**received);
}

Result<std::optional<bytes::Bytes>> channel_request_optional(Context& context, bytes::ByteView pdu, int timeout_ms,
                                                             int delay_ms = 0)
{
    if (const Status checkpoint = cancelled_if_requested(context, "cancelled before CAN request");
        !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    if (const Status sent = context.channel.send(pdu, context.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    if (delay_ms > 0)
    {
        if (const Status slept = context.clock.sleep(delay_ms, context.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    return context.channel.receive(timeout_ms, context.cancellation);
}

Result<bytes::Bytes> beef_exchange(Context& context, bytes::Byte opcode, bytes::ByteView payload,
                                   std::size_t minimum_payload, int timeout_ms)
{
    const bytes::Bytes request = beef_request(opcode, payload);
    Result<bytes::Bytes> reply = channel_request(context, request, timeout_ms);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    Result<BeefMessage> parsed = parse_beef(*reply);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    const bytes::Byte expected_opcode = static_cast<bytes::Byte>(opcode | 0x40U);
    if (parsed->opcode != expected_opcode)
    {
        return fail(ErrorKind::BadResponse, std::format("unexpected BEEF response opcode 0x{:02x}, expected 0x{:02x}",
                                                        parsed->opcode, expected_opcode));
    }
    if (parsed->payload.size() < minimum_payload)
    {
        return fail(ErrorKind::BadResponse, "short BEEF response payload");
    }
    return bytes::Bytes(parsed->payload.begin(), parsed->payload.end());
}

Result<std::optional<std::string>> request_kernel_id(Context& context)
{
    // request_kernel_id(), revision 59f4e442 lines 1583-1647. The literal
    // PDU is intentionally kept here instead of deriving it from BEEF
    // constants: 0x7A 0xA0 is the legacy kernel probe, not a BEEF command.
    const bytes::Bytes request{0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (const Status checkpoint = cancelled_if_requested(context, "kernel ID probe cancelled");
            !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        Result<std::optional<bytes::Bytes>> received =
            channel_request_optional(context, request, kLongTimeoutMs, kProbeDelayMs);
        if (!received.has_value())
        {
            if (received.error().kind == ErrorKind::Timeout)
            {
                continue;
            }
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            continue;
        }

        Result<BeefMessage> parsed = parse_beef(**received);
        if (!parsed.has_value())
        {
            return std::unexpected(parsed.error());
        }
        if (parsed->opcode != kKernelId)
        {
            error(context, std::format("Wrong response from ECU: {}", bytes::toHex(**received)));
            return std::optional<std::string>{};
        }

        return std::optional<std::string>{std::string(parsed->payload.begin(), parsed->payload.end())};
    }
    return std::optional<std::string>{};
}

Result<std::optional<bytes::Bytes>> nonfatal_query(Context& context, bytes::ByteView pdu)
{
    Result<std::optional<bytes::Bytes>> received =
        channel_request_optional(context, pdu, kReadTimeoutMs, kExtraShortTimeoutMs);
    if (!received.has_value())
    {
        if (received.error().kind == ErrorKind::Cancelled || received.error().kind == ErrorKind::Disconnected)
        {
            return std::unexpected(received.error());
        }
        if (received.error().kind == ErrorKind::Timeout)
        {
            error(context, "No valid response from ECU");
            return std::optional<bytes::Bytes>{};
        }
        error(context, std::format("Wrong response from TCU: {}", received.error().detail));
        return std::optional<bytes::Bytes>{};
    }
    if (!received->has_value())
    {
        error(context, "No valid response from ECU");
    }
    return received;
}

Status strict_payload(Context& context, bytes::ByteView pdu, bytes::ByteView expected, std::string_view label,
                      std::optional<std::size_t> minimum_size = std::nullopt)
{
    Result<bytes::Bytes> reply = context.uds.request(pdu, kStrictPolicy, context.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::payload(*reply);
    const std::size_t required = minimum_size.value_or(expected.size());
    if (payload.size() < required || !std::equal(expected.begin(), expected.end(), payload.begin()))
    {
        error(context, std::format("Wrong response from TCU during {}", label));
        return fail(ErrorKind::BadResponse, std::format("unexpected response during {}", label));
    }
    return {};
}

Status strict_seed(Context& context, bytes::Bytes& key)
{
    Result<bytes::Bytes> seed_reply = context.uds.request(
        bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, kStrictPolicy, context.cancellation);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    const bytes::ByteView payload = uds::payload(*seed_reply);
    if (payload.size() < 5 || payload[0] != uds::kSecurityAccessRequestSeed)
    {
        return fail(ErrorKind::BadResponse, "invalid TCU seed response");
    }
    key = seed_key(payload.subspan(1, 4));
    return {};
}

Status connect_bootloader(Context& context, bool read_operation, bool& kernel_alive, std::optional<std::string>& rom_id)
{
    // connect_bootloader(), revision 59f4e442 lines 107-135.
    info(context, "Checking if kernel is already running...");
    info(context, "Requesting kernel ID");
    Result<std::optional<std::string>> kernel_id = request_kernel_id(context);
    if (!kernel_id.has_value())
    {
        return std::unexpected(kernel_id.error());
    }
    if (kernel_id->has_value())
    {
        info(context, std::format("Kernel ID: {}", **kernel_id));
        kernel_alive = true;
        return {};
    }

    error(context, "No valid response from ECU");
    info(context, "No response from kernel, initialising ECU...");

    // The ECU/CAL identity queries (lines 137-214) are intentionally
    // non-fatal. Their successful values form the legacy read ROM id.
    std::optional<std::string> ecu_id;
    info(context, "Requesting ECU ID");
    Result<std::optional<bytes::Bytes>> ecu_reply = nonfatal_query(context, bytes::Bytes{0xAA});
    if (!ecu_reply.has_value())
    {
        return std::unexpected(ecu_reply.error());
    }
    if (ecu_reply->has_value())
    {
        const bytes::Bytes& pdu = **ecu_reply;
        if (pdu.size() >= 9 && pdu[0] == 0xEA)
        {
            ecu_id = std::string(pdu.begin() + 4, pdu.begin() + 9);
            info(context, std::format("ECU ID: {}", *ecu_id));
            if (read_operation)
            {
                rom_id = *ecu_id + "_";
            }
        }
        else
        {
            error(context, std::format("Wrong response from TCU: {}", bytes::toHex(pdu)));
        }
    }

    std::optional<std::string> cal_id;
    info(context, "Requesting CAL ID");
    Result<std::optional<bytes::Bytes>> cal_reply =
        nonfatal_query(context, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId});
    if (!cal_reply.has_value())
    {
        return std::unexpected(cal_reply.error());
    }
    if (cal_reply->has_value())
    {
        const bytes::Bytes& pdu = **cal_reply;
        if (pdu.size() >= 3 && pdu[0] == 0x49 && pdu[1] == 0x04)
        {
            cal_id = std::string(pdu.begin() + 3, pdu.end());
            info(context, std::format("CAL ID: {}", *cal_id));
            if (read_operation)
            {
                rom_id = *cal_id + "_" + rom_id.value_or(std::string{});
            }
        }
        else
        {
            error(context, std::format("Wrong response from TCU: {}", bytes::toHex(pdu)));
        }
    }

    // Strict UDS session/security exchanges, lines 216-359.
    info(context, "Requesting session mode");
    if (const Status session = strict_payload(context, bytes::Bytes{uds::kSidDiagnosticSessionControl, 0x03},
                                              bytes::Bytes{0x03}, "session mode");
        !session.has_value())
    {
        return session;
    }

    bytes::Bytes key;
    if (const Status seed = strict_seed(context, key); !seed.has_value())
    {
        return seed;
    }
    info(context, "Seed request ok");
    info(context, "Sending seed key");
    bytes::Bytes key_request{uds::kSidSecurityAccess, uds::kSecurityAccessSendKey};
    key_request.insert(key_request.end(), key.begin(), key.end());
    if (const Status key_status = strict_payload(context, key_request, bytes::Bytes{0x02}, "seed key");
        !key_status.has_value())
    {
        return key_status;
    }
    info(context, "Seed key ok");

    info(context, "Requesting programming session");
    Result<bytes::Bytes> programming =
        context.uds.request(bytes::Bytes{uds::kSidDiagnosticSessionControl, 0x02}, kStrictPolicy, context.cancellation);
    if (!programming.has_value())
    {
        return std::unexpected(programming.error());
    }
    const bytes::ByteView programming_payload = uds::payload(*programming);
    if (programming_payload.empty() || (programming_payload[0] != 0x02 && programming_payload[0] != 0x42))
    {
        return fail(ErrorKind::BadResponse, "unexpected programming session response");
    }
    info(context, "Succesfully set to programming session");
    return {};
}

Status upload_kernel(Context& context, const KernelImage& kernel)
{
    // upload_kernel(), revision 59f4e442 lines 369-627. The source pads to
    // 128-byte transfer blocks, removes the final four bytes, computes the
    // 0x5AA5A55A complement over 32-bit words, appends it, and encrypts.
    const std::size_t padded_file_size = (kernel.bytes.size() + 3U) & ~std::size_t{3U};
    const std::size_t block_count = (padded_file_size + kKernelUploadBlockSize - 1U) / kKernelUploadBlockSize;
    if (block_count == 0 || block_count > (std::numeric_limits<std::size_t>::max() / kKernelUploadBlockSize))
    {
        return fail(ErrorKind::InvalidConfig, "TCU kernel upload size is invalid");
    }
    const std::size_t data_length = block_count * kKernelUploadBlockSize;
    bytes::Bytes plain = kernel.bytes;
    plain.resize(data_length, bytes::Byte{0});
    if (plain.size() < 4)
    {
        return fail(ErrorKind::InvalidConfig, "TCU kernel upload is shorter than its checksum word");
    }
    plain.resize(plain.size() - 4);
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset < plain.size(); offset += 4)
    {
        sum += bytes::readU32Be(plain, offset);
    }
    bytes::appendU32Be(plain, 0x5AA5A55AU - sum);
    const bytes::Bytes encrypted = encrypt_payload(plain);

    debug(context, std::format("Start address to upload kernel: 0x{:x}", kernel.load_address));
    info(context, "Initialize kernel upload");
    if (const Status request_download =
            strict_payload(context,
                           composeBe(0x34_b, 0x04_b, 0x33_b, bytes::u24(kernel.load_address),
                                     bytes::u24(static_cast<std::uint32_t>(data_length))),
                           bytes::Bytes{0x20}, "kernel download");
        !request_download.has_value())
    {
        return request_download;
    }

    info(context, "Uploading kernel, please wait...");
    std::size_t remaining = data_length;
    for (std::size_t block = 0; block <= block_count; ++block)
    {
        if (const Status checkpoint = cancelled_if_requested(context, "kernel upload cancelled");
            !checkpoint.has_value())
        {
            return checkpoint;
        }
        const std::size_t block_offset = block * kKernelUploadBlockSize;
        const std::size_t chunk_size = block == block_count ? remaining : kKernelUploadBlockSize;
        const std::uint32_t address = kernel.load_address + static_cast<std::uint32_t>(block_offset);
        const bytes::ByteView chunk(encrypted.data() + static_cast<std::ptrdiff_t>(block_offset), chunk_size);
        const bytes::Bytes request = composeBe(0xB6_b, bytes::u24(address), chunk);
        // Legacy reads the transfer-block response but does not inspect it
        // (lines 508-513); a missing frame is consequently tolerated.
        Result<std::optional<bytes::Bytes>> ignored = channel_request_optional(context, request, kReadTimeoutMs);
        if (!ignored.has_value())
        {
            return std::unexpected(ignored.error());
        }
        if (block < block_count)
        {
            remaining -= kKernelUploadBlockSize;
        }
    }

    info(context, "Kernel uploaded, starting...");
    if (const Status transfer_exit =
            strict_payload(context, bytes::Bytes{uds::kSidRequestTransferExit}, {}, "kernel transfer exit");
        !transfer_exit.has_value())
    {
        return transfer_exit;
    }
    if (const Status start_kernel =
            strict_payload(context, bytes::Bytes{uds::kSidRoutineControl, 0x01, 0x02, 0x02, 0x02},
                           bytes::Bytes{0x01, 0x02, 0x02, 0x02}, "kernel start");
        !start_kernel.has_value())
    {
        return start_kernel;
    }

    info(context, "Kernel started, initializing...");
    if (const Status slept = context.clock.sleep(kPostUploadDelayMs, context.cancellation); !slept.has_value())
    {
        return slept;
    }
    info(context, "Requesting kernel ID");
    Result<std::optional<std::string>> kernel_id = request_kernel_id(context);
    if (!kernel_id.has_value())
    {
        if (kernel_id.error().kind != ErrorKind::Cancelled && kernel_id.error().kind != ErrorKind::Disconnected)
        {
            error(context, "No valid response from ECU");
            return {};
        }
        return std::unexpected(kernel_id.error());
    }
    if (kernel_id->has_value())
    {
        info(context, std::format("Kernel ID: {}", **kernel_id));
    }
    else
    {
        error(context, "No valid response from ECU");
    }
    return {};
}

Result<bytes::Bytes> read_memory(Context& context, const FlashPlan& plan, PhaseReporter& progress)
{
    // read_mem(), revision 59f4e442 lines 632-779. The kernel reads fixed
    // 0x400-byte pages and each response is decrypted with the reverse SSM
    // table before being returned to the caller.
    const MemoryRegion region = plan.transfer_region();
    bytes::Bytes rom;
    rom.reserve(region.length);
    info(context, "Start reading ROM, please wait...");
    for (std::uint32_t offset = 0; offset < region.length; offset += kKernelPageSize)
    {
        const std::uint64_t loop_started_ms = context.clock.now_ms();
        if (const Status checkpoint = cancelled_if_requested(context, "read cancelled"); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const std::uint32_t address = region.start + offset;
        const bytes::Bytes payload = composeBe(bytes::Byte{0x00}, bytes::u24(address), std::uint16_t{kKernelPageSize});
        Result<bytes::Bytes> reply = beef_exchange(context, kKernelReadArea, payload, kKernelPageSize, kReadTimeoutMs);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        bytes::Bytes page = decrypt_payload(bytes::ByteView(*reply).first(kKernelPageSize));
        if (page.size() > region.length - offset)
        {
            page.resize(region.length - offset);
        }
        std::uint64_t elapsed_ms = context.clock.now_ms() - loop_started_ms;
        if (elapsed_ms == 0)
        {
            elapsed_ms = 1;
        }
        unsigned speed = static_cast<unsigned>(kKernelPageSize * (1000.0F / static_cast<float>(elapsed_ms)));
        if (speed == 0)
        {
            speed = 1;
        }
        const unsigned time_left = static_cast<unsigned>(((region.length - offset) / speed) % 9999U) + 1U;
        info(context, std::format("Kernel read addr: 0x{:08X} length: 0x{:08X}, {:>6} B/s {:>6} s", address,
                                  kKernelPageSize, speed, time_left));
        rom.insert(rom.end(), page.begin(), page.end());
        progress.update(static_cast<int>(std::min<std::uint64_t>(region.length, offset + kKernelPageSize)));
    }
    info(context, "ROM read ready");
    return rom;
}

struct CompareResult
{
    std::vector<bool> modified;
    std::size_t changed_count = 0;
};

Result<std::uint32_t> query_crc(Context& context, const MemoryRegion& block)
{
    // check_romcrc(), revision 59f4e442 lines 942-1024. The short read after
    // a comparison is a stale-frame drain and is deliberately retained.
    const bytes::Bytes payload = composeBe(block.start, bytes::Byte{0x00}, bytes::u24(block.length));
    Result<bytes::Bytes> reply = beef_exchange(context, kKernelCrc, payload, 4, kExtraLongTimeoutMs);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const std::uint32_t crc = bytes::readU32Be(*reply);
    Result<std::optional<bytes::Bytes>> stale = context.channel.receive(kShortTimeoutMs, context.cancellation);
    if (!stale.has_value())
    {
        return std::unexpected(stale.error());
    }
    return crc;
}

Result<CompareResult> compare_blocks(Context& context, const FlashPlan& plan, bytes::ByteView image,
                                     bool after_reflash = false)
{
    // write_mem()/get_changed_blocks(), revision 59f4e442 lines 784-935.
    CompareResult result;
    result.modified.assign(plan.erase_regions().size(), false);

    info(context, after_reflash ? "--- Comparing ECU flash memory pages to image file after reflash ---"
                                : "--- Comparing ECU flash memory pages to image file ---");
    info(context, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    for (std::size_t index = 0; index < plan.erase_regions().size(); ++index)
    {
        if (const Status checkpoint = cancelled_if_requested(context, "ROM compare cancelled"); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const MemoryRegion block = plan.erase_regions()[index];
        if (block.start < plan.transfer_region().start ||
            static_cast<std::uint64_t>(block.start - plan.transfer_region().start) + block.length > image.size())
        {
            return fail(ErrorKind::InvalidConfig, "TCU CRC block is outside the image");
        }
        info(context, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
        Result<std::uint32_t> ecu_crc = query_crc(context, block);
        if (!ecu_crc.has_value())
        {
            return std::unexpected(ecu_crc.error());
        }
        const std::size_t image_offset = block.start - plan.transfer_region().start;
        const std::uint32_t image_crc = fastecu::checksum::crc32(image.subspan(image_offset, block.length));
        debug(context, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", *ecu_crc, image_crc));
        info(context, std::format("\t{:08X}\t{:08X}", *ecu_crc, image_crc));
        result.modified[index] = *ecu_crc != image_crc;
        info(context, result.modified[index] ? "\tNO" : "\tYES");
        if (result.modified[index])
        {
            ++result.changed_count;
        }
        if (const Status slept = context.clock.sleep(5, context.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }

    info(context, "Different blocks : ");
    for (std::size_t index = 0; index < result.modified.size(); ++index)
    {
        if (result.modified[index])
        {
            info(context, std::format("{}, ", index));
        }
    }
    info(context, std::format(" (total: {})", result.changed_count));
    return result;
}

Status initialize_flash(Context& context)
{
    // init_flash_write(), revision 59f4e442 lines 1027-1175.
    info(context, "Check max message length");
    Result<bytes::Bytes> max_message = beef_exchange(context, kKernelGetMaxMessage, {}, 4, kReadTimeoutMs);
    if (!max_message.has_value())
    {
        return std::unexpected(max_message.error());
    }
    info(context, std::format(": 0x{:04X}", bytes::readU32Be(*max_message)));

    info(context, "Check flashblock size");
    Result<bytes::Bytes> max_block = beef_exchange(context, kKernelGetMaxBlock, {}, 4, kReadTimeoutMs);
    if (!max_block.has_value())
    {
        return std::unexpected(max_block.error());
    }
    info(context, std::format(": 0x{:04X}", bytes::readU32Be(*max_block)));

    info(context, "Test write mode off, perform actual flash write");
    Result<bytes::Bytes> enabled = beef_exchange(context, kKernelFlashEnable, {}, 0, kReadTimeoutMs);
    if (!enabled.has_value())
    {
        return std::unexpected(enabled.error());
    }
    error(context, "Flash mode succesfully set");
    return {};
}

Status flash_block(Context& context, bytes::ByteView image, const FlashPlan& plan, const MemoryRegion& block,
                   std::size_t& flash_bytes_index, std::size_t flash_bytes_count, PhaseReporter& progress)
{
    // flash_block(), revision 59f4e442 lines 1270-1515. The legacy always
    // uses 0x200-byte buffer writes and commits each 0x1000-byte page.
    if (block.length == 0 || block.length % kFlashBufferSize != 0 || block.length % kFlashCommitSize != 0)
    {
        return fail(ErrorKind::InvalidConfig, "TCU flash block is not aligned to the portable write windows");
    }
    const std::size_t image_offset = block.start - plan.transfer_region().start;
    std::uint32_t start = block.start;
    std::uint32_t remaining = block.length;
    std::uint32_t commit_start = block.start;
    std::uint64_t previous_time = 0;

    info(context, std::format("Flash page erase addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    info(context, "Erasing flash page...");
    const bytes::Bytes erase_payload = composeBe(block.start);
    Result<bytes::Bytes> erased = beef_exchange(context, kKernelBlankPage, erase_payload, 0, kExtraLongTimeoutMs);
    if (!erased.has_value())
    {
        return std::unexpected(erased.error());
    }
    info(context, " erased");
    previous_time = context.clock.now_ms();

    info(context, std::format("Start flash write addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    while (remaining != 0)
    {
        if (const Status checkpoint = cancelled_if_requested(context, "flash write cancelled"); !checkpoint.has_value())
        {
            return checkpoint;
        }
        const std::size_t chunk_offset = static_cast<std::size_t>(start - block.start);
        const bytes::ByteView chunk = image.subspan(image_offset + chunk_offset, kFlashBufferSize);
        const bytes::Bytes write_payload = composeBe(start, chunk);
        Result<bytes::Bytes> written = beef_exchange(context, kKernelWriteBuffer, write_payload, 0, 800);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        debug(context, "Data written to flash buffer");

        const std::uint32_t percent =
            static_cast<std::uint32_t>(100U * (block.length - remaining) / std::max<std::uint32_t>(block.length, 1));
        const std::uint64_t now = context.clock.now_ms();
        std::uint64_t elapsed = now >= previous_time ? now - previous_time : 0;
        if (elapsed == 0)
        {
            elapsed = 1;
        }
        previous_time = now;
        std::uint32_t speed = static_cast<std::uint32_t>(kFlashBufferSize * 1000U / elapsed);
        if (speed == 0)
        {
            speed = 1;
        }
        const std::uint32_t next_index = static_cast<std::uint32_t>(flash_bytes_index + kFlashBufferSize);
        std::uint32_t time_left = static_cast<std::uint32_t>((flash_bytes_count - next_index) / speed);
        if (time_left > 9999)
        {
            time_left = 9999;
        }
        ++time_left;
        info(context,
             std::format("Write flash buffer: 0x{:08X} ({}% - {} B/s, ~ {} s)", start, percent, speed, time_left));

        remaining -= kFlashBufferSize;
        start += kFlashBufferSize;
        flash_bytes_index += kFlashBufferSize;
        progress.update(static_cast<int>(std::min<std::size_t>(flash_bytes_index, flash_bytes_count)));

        if (start - commit_start == kFlashCommitSize)
        {
            if (const Status checkpoint = cancelled_if_requested(context, "flash commit cancelled");
                !checkpoint.has_value())
            {
                return checkpoint;
            }
            info(context, "Flash buffer write complete... ");
            const std::size_t crc_offset = image_offset + static_cast<std::size_t>(commit_start - block.start);
            const std::uint32_t image_crc = fastecu::checksum::crc32(image.subspan(crc_offset, kFlashCommitSize));
            debug(context, std::format("Image CRC32: 0x{:08x}", image_crc));
            info(context, std::format("Committ flash addr: 0x{:x}", commit_start));
            info(context, std::format(" len: 0x{:x}", kFlashCommitSize));
            info(context, std::format(" crc32: 0x{:08x}", image_crc));
            const bytes::Bytes commit_payload = composeBe(commit_start, std::uint16_t{kFlashCommitSize}, image_crc);
            Result<bytes::Bytes> committed =
                beef_exchange(context, kKernelCommitBuffer, commit_payload, 0, kExtraLongTimeoutMs);
            if (!committed.has_value())
            {
                return std::unexpected(committed.error());
            }
            commit_start += kFlashCommitSize;
        }
    }
    return {};
}

Status reflash_block(Context& context, bytes::ByteView image, const FlashPlan& plan, const MemoryRegion& block,
                     std::size_t& flash_bytes_index, std::size_t flash_bytes_count, PhaseReporter& progress)
{
    // reflash_block(), revision 59f4e442 lines 1183-1267. The voltage query
    // precedes flash_block and a failure after erase receives the exact
    // operator recovery warning from the legacy worker.
    info(context, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    info(context, "Check flash voltage");
    Result<bytes::Bytes> voltage = beef_exchange(context, kKernelProgVolt, {}, 2, kMediumTimeoutMs);
    if (!voltage.has_value())
    {
        return std::unexpected(voltage.error());
    }
    const double volts = static_cast<double>(bytes::readU16Be(*voltage)) / 50.0;
    info(context, std::format(": {}V", volts));

    Status flashed = flash_block(context, image, plan, block, flash_bytes_index, flash_bytes_count, progress);
    if (!flashed.has_value())
    {
        error(context, "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still "
                       "running and receiving commands!");
        return flashed;
    }
    info(context, "Flash block ok");
    return {};
}

Status write_memory(Context& context, const FlashPlan& plan, PhaseReporter& progress)
{
    // write_mem(), revision 59f4e442 lines 784-900.
    const bytes::ByteView image = *plan.image();
    Result<CompareResult> before = compare_blocks(context, plan, image);
    if (!before.has_value())
    {
        return std::unexpected(before.error());
    }
    if (before->changed_count == 0)
    {
        info(context, "*** Compare results no difference between ROM and ECU data, no flashing needed! ***");
        return {};
    }

    info(context, "--- Start writing ROM file to ECU flash memory ---");
    if (const Status initialized = initialize_flash(context); !initialized.has_value())
    {
        return initialized;
    }
    std::size_t flash_bytes_count = 0;
    for (std::size_t index = 0; index < before->modified.size(); ++index)
    {
        if (before->modified[index])
        {
            flash_bytes_count += plan.erase_regions()[index].length;
        }
    }
    std::size_t flash_bytes_index = 0;
    for (std::size_t index = 0; index < before->modified.size(); ++index)
    {
        if (!before->modified[index])
        {
            continue;
        }
        if (const Status checkpoint = cancelled_if_requested(context, "flash block loop cancelled");
            !checkpoint.has_value())
        {
            return checkpoint;
        }
        if (const Status reflashed = reflash_block(context, image, plan, plan.erase_regions()[index], flash_bytes_index,
                                                   flash_bytes_count, progress);
            !reflashed.has_value())
        {
            info(context, std::format("Block {} reflash failed.", index));
            return reflashed;
        }
        info(context, std::format("Block {} reflash complete.", index));
    }

    Result<CompareResult> after = compare_blocks(context, plan, image, true);
    if (!after.has_value())
    {
        return std::unexpected(after.error());
    }
    if (after->changed_count != 0)
    {
        error(context, "*** ERROR IN FLASH PROCESS ***");
        error(context, "Don't power off your ECU, kernel is still running and you can try flashing again!");
    }
    return {};
}

} // namespace

Result<Iso15765Config> SubaruTcuDensoSh705xCanExecutor::transport_setup(const FlashPlan& plan) const
{
    if (const Status matched = check_family(plan, FlashFamily::SubaruTcuDensoSh705xCan); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = validate_subaru_tcu_denso_sh705x_can_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& family = std::get<SubaruTcuDensoSh705xCanPlan>(plan.family_plan());
    return iso15765_config_from(family);
}

Result<FlashExecutionResult> SubaruTcuDensoSh705xCanExecutor::execute(const FlashPlan& plan,
                                                                      ICanFlashTransport& transport, IClock& clock,
                                                                      const ICancellationToken& cancellation,
                                                                      IEventSink& events)
{
    if (const Status matched = check_family(plan, FlashFamily::SubaruTcuDensoSh705xCan); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = validate_subaru_tcu_denso_sh705x_can_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruTcuDensoSh705xCanPlan>(plan.family_plan());
    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Context context{cancellation, events, clock, uds_client, channel};

    const bool read_operation = plan.operation() == FlashOperation::Read;
    PhaseSequence phases(events, read_operation ? 2 : 3);
    PhaseReporter kernel_phase = phases.start("Kernel", 1);
    bool kernel_alive = false;
    std::optional<std::string> rom_id;
    if (const Status connected = connect_bootloader(context, read_operation, kernel_alive, rom_id);
        !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    kernel_phase.complete();

    if (!kernel_alive && plan.kernel().has_value())
    {
        // Stock ECU path: connect_bootloader already ran strict session and
        // security exchanges, so upload the caller-owned kernel now.
        if (const Status uploaded = upload_kernel(context, *plan.kernel()); !uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
    }

    if (read_operation)
    {
        PhaseReporter read_phase = phases.start("Read", static_cast<int>(plan.transfer_region().length));
        Result<bytes::Bytes> rom = read_memory(context, plan, read_phase);
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        read_phase.complete();
        return FlashExecutionResult{
            .operation = FlashOperation::Read, .read_bytes = std::move(*rom), .rom_id = std::move(rom_id)};
    }

    PhaseReporter write_phase = phases.start("Write", static_cast<int>(plan.transfer_region().length));
    if (const Status written = write_memory(context, plan, write_phase); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    write_phase.complete();
    PhaseReporter complete = phases.start("Complete", 1);
    complete.complete();
    return FlashExecutionResult{.operation = FlashOperation::Write};
}

} // namespace fastecu::flash
