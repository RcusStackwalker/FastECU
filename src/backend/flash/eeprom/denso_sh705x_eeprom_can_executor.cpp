#include "src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.h"

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;

// ---------------------------------------------------------------------
// Literal protocol constants transcribed from
// src/backend/definitions/kernelcomms.h (not #included: the K-Line sibling
// set this precedent of transcribing rather than adding a new dependency).
// ---------------------------------------------------------------------
constexpr std::uint16_t kSubKernelStartComm = 0xbeef; // SUB_KERNEL_START_COMM
constexpr std::uint8_t kSubKernelId = 0x01;           // SUB_KERNEL_ID
constexpr std::uint8_t kSubKernelReadArea = 0x03;     // SUB_KERNEL_READ_AREA
constexpr std::uint8_t kSubKernelReadEeprom = 0x07;   // SUB_KERNEL_READ_EEPROM

constexpr std::uint32_t kUploadChunkBytes = 128;     // upload_kernel()'s 0xB6 block size
constexpr std::uint32_t kMaxEepromPageBytes = 0x400; // read_mem()'s pagesize cap

// Every timeout/delay below is transcribed from a specific
// eeprom_ecu_subaru_denso_sh705x_can_operation.cpp call site (grep-verified
// against the deleted file); several deliberately differ from the K-Line
// sibling's own constants of the same apparent purpose -- see task-9-report.md
// for the full site-by-site mapping.
constexpr int kHandshakeDelayMs = 50;         // delay(50) before every connect_bootloader() read
constexpr int kHandshakeTimeoutMs = 2000;     // serial_read_timeout
constexpr int kKernelIdRequestDelayMs = 200;  // request_kernel_id():1382 delay(200)
constexpr int kKernelIdReadTimeoutMs = 800;   // serial_read_long_timeout
constexpr int kQuickAckTimeoutMs = 10;        // literal "10" -- upload_kernel()'s SID34/0x37/0x31 ack reads
constexpr int kBlockAckTimeoutMs = 500;       // receive_timeout -- 0xB6 block acks (no delay before)
constexpr int kPostUploadSettleDelayMs = 100; // upload_kernel():892 delay(100)
constexpr int kPageHeaderTimeoutMs = 2000;    // serial_read_timeout -- read_mem()'s header-ack read (no delay before)
constexpr int kPagedataPollTimeoutMs = 200;   // serial_read_short_timeout
constexpr int kMaxPagedataAttempts = 100;     // read_mem()'s inner accumulation loop cap (line 1056: `timeout < 100`)
constexpr int kInterPageDelayMs = 1;          // read_mem():1117 delay(1)

// ---------------------------------------------------------------------
// Request builders. Every UDS-over-CAN request in this legacy class (except
// the out-of-scope read_ram_location()) is built inline with a literal
// 4-byte "CAN ID" prefix -- [0x00,0x00,0x07,0xE0] in the legacy source,
// which is exactly can_plan.request_id (0x7e0) encoded big-endian in 4
// bytes. Threading request_id through here (not hardcoding the legacy
// literal) is the CAN analogue of the kernel_baud lesson from Task 8's
// review: a future ROM/ECU pair with a different request_id must not
// silently keep using 0x7e0.
// ---------------------------------------------------------------------

bytes::Bytes can_frame(std::uint32_t request_id, bytes::ByteView payload)
{
    return composeBe(request_id, payload);
}

bytes::Bytes init_connection_request(std::uint32_t request_id)
{
    return can_frame(request_id, bytes::Bytes{0x01, 0x00});
}
bytes::Bytes ecu_id_request(std::uint32_t request_id)
{
    return can_frame(request_id, bytes::Bytes{0xAA});
}
bytes::Bytes vin_request(std::uint32_t request_id)
{
    return can_frame(request_id, bytes::Bytes{0x09, 0x02});
}
bytes::Bytes cal_id_request(std::uint32_t request_id)
{
    return can_frame(request_id, bytes::Bytes{0x09, 0x04});
}
bytes::Bytes cvn_request(std::uint32_t request_id)
{
    return can_frame(request_id, bytes::Bytes{0x09, 0x06});
}
bytes::Bytes session_mode_request(std::uint32_t request_id, std::uint8_t mode)
{
    return can_frame(request_id, bytes::Bytes{0x10, static_cast<bytes::Byte>(mode)});
}
bytes::Bytes seed_request(std::uint32_t request_id)
{
    return can_frame(request_id, bytes::Bytes{0x27, 0x01});
}
bytes::Bytes seed_key_send_request(std::uint32_t request_id, bytes::ByteView key)
{
    return can_frame(request_id, composeBe(0x27_b, 0x02_b, key));
}
// lines 586-602: [0x10] with 0x02 appended iff req_10_03_connected, then 0x42
// appended iff req_10_43_connected. Computed dynamically, not hardcoded to
// "both true" -- the two prior session-mode checks (586-602) never hard-fail
// on mismatch, so either flag can legitimately end up false.
bytes::Bytes session_set_request(std::uint32_t request_id, bool req_10_03_connected, bool req_10_43_connected)
{
    bytes::Bytes payload{0x10};
    if (req_10_03_connected)
    {
        payload.push_back(0x02);
    }
    if (req_10_43_connected)
    {
        payload.push_back(0x42);
    }
    return can_frame(request_id, payload);
}
// request_kernel_id(), lines 1355-1390: UNLIKE the K-Line sibling's
// request_kernel_id(), this one has NO trailing checksum byte.
bytes::Bytes request_kernel_id_frame(std::uint32_t request_id)
{
    // Trailing 3 zero bytes (0x00,0x00,0x00) are NOT checksum -- see the
    // comment above this function -- they're literal payload padding present
    // in the legacy frame; composeBe can't infer them from kSubKernelId's
    // type, so they're spelled out explicitly to keep the 8-byte payload
    // length exact.
    return can_frame(request_id,
                     composeBe(kSubKernelStartComm, std::uint16_t{1}, kSubKernelId, 0x00_b, 0x00_b, 0x00_b));
}
bytes::Bytes sid34_request(std::uint32_t request_id, std::uint32_t addr, std::uint32_t data_len)
{
    return can_frame(request_id, composeBe(0x34_b, 0x04_b, 0x33_b, u24(addr), u24(data_len)));
}
bytes::Bytes sid_b6_request(std::uint32_t request_id, std::uint32_t block_addr, bytes::ByteView chunk)
{
    return can_frame(request_id, composeBe(0xB6_b, u24(block_addr), chunk));
}
bytes::Bytes sid37_request(std::uint32_t request_id)
{
    return can_frame(request_id, bytes::Bytes{0x37});
}
bytes::Bytes sid31_request(std::uint32_t request_id)
{
    return can_frame(request_id, bytes::Bytes{0x31, 0x01, 0x02, 0x02, 0x02});
}
// read_mem(), lines 987-1004 (template) + 1029-1033 (per-iteration overwrite):
// datalen==6 hardcoded.
bytes::Bytes read_eeprom_request(std::uint32_t request_id, std::uint8_t mode, std::uint32_t addr,
                                 std::uint32_t pagesize)
{
    constexpr std::uint32_t kDatalen = 6;
    return can_frame(request_id, composeBe(kSubKernelStartComm, std::uint16_t(kDatalen + 1), kSubKernelReadEeprom, mode,
                                           u24(addr), std::uint16_t(pagesize)));
}

// request_kernel_id()'s / upload_kernel()'s post-upload poll's "kernel alive"
// marker check, lines 176,934: received[4..5] == SUB_KERNEL_START_COMM
// big-endian, received[8] == SUB_KERNEL_ID | 0x40.
bool looks_kernel_alive(bytes::ByteView received)
{
    return received.size() > 8 && bytes::readU16Be(received, 4) == kSubKernelStartComm &&
           received[8] == static_cast<bytes::Byte>(kSubKernelId | 0x40);
}

// ---------------------------------------------------------------------
// Seed-key algorithms -- transcribed from each generate_*_seed_key() body.
// This class's tables are its OWN, distinct from the K-Line sibling's
// (Task 7 report, "Legacy behavior surprises" #8) -- verified against
// task-7-report.md's independently re-transcribed tables, not copy-pasted
// from the K-Line executor.
// ---------------------------------------------------------------------

// generate_seed_key(), lines 1146-1172 (Stock).
bytes::Bytes generate_stock_seed_key(bytes::ByteView seed)
{
    static constexpr std::uint16_t kIndex[] = {0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                               0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
    static constexpr std::uint8_t kTransform[] = {0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                  0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                  0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}

// generate_ecutek_seed_key(), lines 1224-1269 (base calculateSeedKey() call
// only, lines 1228-1247 -- the "_ecutek_racerom_alt" post-processing,
// 1249-1266, is intentionally not reproduced; see the OPEN QUESTION
// resolution comment above connect_bootloader() below). Same
// keytogenerateindex_1 as Stock; indextransformation's first 5 entries
// differ (0x4,0x2,0x5,0x1,0x8 vs Stock's 0x5,0x6,0x7,0x1,0x9).
bytes::Bytes generate_ecutek_seed_key(bytes::ByteView seed)
{
    static constexpr std::uint16_t kIndex[] = {0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                               0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
    static constexpr std::uint8_t kTransform[] = {0x4, 0x2, 0x5, 0x1, 0x8, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                  0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                  0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}

// generate_cobb_seed_key(), lines 1274-1302 ("2017 VA model" table -- the one
// actually passed to calculateSeedKey()).
bytes::Bytes generate_cobb_seed_key(bytes::ByteView seed)
{
    static constexpr std::uint16_t kIndex[] = {0x9DDB, 0x9CFB, 0x9B9A, 0x6136, 0x59E1, 0xBA03, 0xD683, 0x7092,
                                               0x9E05, 0x8723, 0xF998, 0x15BB, 0xB8D5, 0xFF0C, 0x9D91, 0x24B9};
    static constexpr std::uint8_t kTransform[] = {0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                  0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                  0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}

// decrypt_racerom_seed(), lines 1175-1189: plain modular exponentiation
// (square-and-multiply) -- no SsmProtocol equivalent, replicated verbatim.
std::uint64_t decrypt_racerom_seed(std::uint64_t base, std::uint64_t exponent, std::uint64_t modulus)
{
    std::uint64_t result = 1;
    base = base % modulus;
    while (exponent > 0)
    {
        if (exponent & 1)
        {
            result = (result * base) % modulus;
        }
        base = (base * base) % modulus;
        exponent /= 2;
    }
    return result;
}

// generate_ecutek_racerom_can_seed_key(), lines 1191-1217: seed packed
// big-endian into a uint32, RSA-decrypted with hardcoded d/n, re-emitted
// big-endian as the 4-byte key.
bytes::Bytes generate_ecutek_racerom_can_seed_key(bytes::ByteView seed)
{
    const std::uint32_t seed_word = (static_cast<std::uint32_t>(seed[0]) << 24) |
                                    (static_cast<std::uint32_t>(seed[1]) << 16) |
                                    (static_cast<std::uint32_t>(seed[2]) << 8) | static_cast<std::uint32_t>(seed[3]);
    constexpr std::uint64_t d = 0x0A863281ULL;
    constexpr std::uint64_t n = 0x0fda9293ULL;
    const std::uint32_t decrypted = static_cast<std::uint32_t>(decrypt_racerom_seed(seed_word, d, n));
    return composeBe(decrypted);
}

// encrypt_payload(), lines 1314-1330: this class's OWN key table --
// {0xC85B, 0x32C0, 0xE282, 0x92A0} -- distinct from the K-Line sibling's
// ({0x7856, 0xCE22, 0xF513, 0x6E86}).
bytes::Bytes encrypt_can_kernel_payload(bytes::ByteView buf, std::uint32_t len)
{
    static constexpr std::uint16_t kIndex[] = {0xC85B, 0x32C0, 0xE282, 0x92A0};
    static constexpr std::uint8_t kTransform[] = {0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                  0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                  0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculatePayload(buf, len, kIndex, kTransform);
}

// ---------------------------------------------------------------------
// Shared write/read helpers.
// ---------------------------------------------------------------------

// The common write -> [delay] -> read shape shared by every exchange in this
// class. Cancellation is checked before the write, after the write, after
// the delay (if any), and after the read, per the portable seam's
// cancellation contract. A genuine transport-level failure (Status/Result
// carrying an error) always propagates; an absent response ("no frame", the
// nullopt case) is returned as-is for the caller to interpret -- see the two
// call-site wrappers below, since legacy's own tolerance for "no frame" vs.
// "must have gotten something" differs per call site.
Result<std::optional<bytes::Bytes>> can_raw_exchange(ICanFlashTransport& transport, IClock& clock,
                                                     const ICancellationToken& cancellation, bytes::ByteView request,
                                                     int delay_ms, int timeout_ms)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before write");
    }
    if (Status written = transport.write(request, cancellation); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after write");
    }
    if (delay_ms > 0)
    {
        if (Status slept = clock.sleep(delay_ms, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled after delay");
        }
    }
    auto received = transport.read(timeout_ms, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after read");
    }
    return std::move(*received);
}

// Gated variant: an absent response maps to ErrorKind::Timeout. Used only at
// connect_bootloader()'s 3 hard-gating steps (seed request, seed-key send,
// session set) and upload_kernel()'s SID34/0x37/0x31 acks -- exactly the
// sites where legacy's own `if (received.length() > N) {...} else { return
// STATUS_ERROR; }` shape means a short-or-absent response is a hard failure.
Result<bytes::Bytes> can_exchange_gated(ICanFlashTransport& transport, IClock& clock,
                                        const ICancellationToken& cancellation, bytes::ByteView request, int delay_ms,
                                        int timeout_ms)
{
    Result<std::optional<bytes::Bytes>> raw =
        can_raw_exchange(transport, clock, cancellation, request, delay_ms, timeout_ms);
    if (!raw.has_value())
    {
        return std::unexpected(raw.error());
    }
    if (!raw->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU");
    }
    return std::move(**raw);
}

// request_kernel_id(), lines 1355-1390 (write) + 1382-1383 (delay(200) then
// read(serial_read_long_timeout)). An empty/absent response here is NOT an
// error -- it's the normal "kernel not (yet) running" signal that both
// connect_bootloader()'s initial probe and upload_kernel()'s single
// post-upload poll interpret themselves.
Result<std::optional<bytes::Bytes>> request_kernel_id(ICanFlashTransport& transport, IClock& clock,
                                                      const ICancellationToken& cancellation, std::uint32_t request_id)
{
    return can_raw_exchange(transport, clock, cancellation, request_kernel_id_frame(request_id),
                            kKernelIdRequestDelayMs, kKernelIdReadTimeoutMs);
}

} // namespace

Result<Iso15765Config> DensoSh705xEepromCanExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::DensoSh705xEepromCan); !match.has_value())
    {
        return std::unexpected(match.error());
    }

    const auto& can_plan = std::get<DensoSh705xEepromCanPlan>(plan.family_plan());
    return iso15765_config_from(can_plan);
}

Result<FlashExecutionResult> DensoSh705xEepromCanExecutor::execute(const FlashPlan& plan, ICanFlashTransport& transport,
                                                                   IClock& clock,
                                                                   const ICancellationToken& cancellation,
                                                                   IEventSink& events)
{
    if (Status match = check_family(plan, FlashFamily::DensoSh705xEepromCan); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    const auto& can_plan = std::get<DensoSh705xEepromCanPlan>(plan.family_plan());
    ICanFlashTransport& can_transport = transport;
    bool kernel_alive = false;
    if (Status connected = connect_bootloader(can_transport, clock, cancellation, events, can_plan, kernel_alive);
        !connected.has_value())
    {
        return std::unexpected(connected.error());
    }

    if (!kernel_alive)
    {
        // Safe to dereference: this executor only ever runs against
        // DensoSh705xEepromCan plans, and validate_and_build rejects any plan
        // for that family whose kernel is absent (flash_validation.cpp:
        // "family requires a kernel image"). check_family above confirms
        // the family; it does not itself
        // guarantee a kernel -- validate_and_build is what does.
        if (Status uploaded = upload_kernel(can_transport, clock, cancellation, events, can_plan, *plan.kernel());
            !uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
    }

    Result<bytes::Bytes> read_result = read_mem(can_transport, clock, cancellation, events, plan.transfer_region(),
                                                can_plan.mode, can_plan.request_id);

    if (!read_result.has_value())
    {
        return std::unexpected(read_result.error());
    }
    return FlashExecutionResult{
        .operation = plan.operation(),
        .read_bytes = std::move(*read_result),
    };
}

// ---------------------------------------------------------------------
// Resolution of Task 4's OPEN QUESTION (denso_sh705x_eeprom_common.cpp): the
// legacy "_ecutek_racerom_alt" flash-method branch (connect_bootloader(),
// lines 195-234) is intentionally NOT implemented below. Full reasoning is
// in task-9-report.md; summary:
//
//   1. DensoSecurityVariant (fixed at 4 values by Task 4, before this task)
//      has no representation for "_ecutek_racerom_alt" -- and adding a 5th
//      value or a separate plan field would only be meaningful once this
//      executor could actually perform the branch's prerequisite step: a
//      temporary K-Line-shaped SSM exchange (read_ram_location(), addHeader-
//      framed, no CAN-ID prefix) multiplexed over the SAME physical adapter
//      the plan declares as TransportKind::CanIso15765. Nothing in the
//      portable transport seam (ICanFlashTransport/Iso15765Config) exposes a
//      "become K-Line for a moment" capability; inventing one is a
//      transport-architecture change well beyond this task's scope (build
//      the CAN executor), not a per-security-variant branch.
//   2. Even setting (1) aside, Task 7's characterization work could only pin
//      read_ram_location()'s FAILURE path (a short/malformed response taking
//      its early `return STATUS_ERROR` branch, lines 680-685) -- the
//      "success" path (lines 671-679, 687-693) reads its result from
//      `response`, a QByteArray that is never populated anywhere in the
//      function (the real bytes land in `received`), so on real hardware a
//      well-formed ACK there is undefined behavior (an out-of-bounds
//      QByteArray read), not a stable, reproducible contract. There is no
//      golden trace for what the resulting seed key should be in that case,
//      so implementing that pipeline now would mean inventing unverified
//      behavior -- exactly what Task 7 declined to do, and exactly what the
//      design spec's compatibility-contract rule forbids guessing at.
//
// This is resolution option (a) from the OPEN QUESTION comment: the RAM-
// location-read path is out of scope for step 5c. connect_bootloader() below
// therefore implements only the 4 branches DensoSecurityVariant can express
// (Stock/EcuTek/Cobb/EcuTekRaceRom) via an exhaustive switch with NO default
// case -- so a 5th security variant added in the future fails to compile
// here until this function is updated, rather than silently falling through
// to the wrong cryptographic function (the one behavioral bug this task's
// brief explicitly forbids).
//
// The one piece of this branch's documented bug this task's brief still asks
// to be fixed on the record: had this branch been implemented,
// read_ram_location() would need to read its result from the POPULATED
// response buffer (`received` in legacy's naming), not the never-populated
// one (`response`) -- see task-7-report.md, "Legacy behavior surprises" #6.
// Recorded here for whoever eventually designs the transport-mode-switching
// capability this branch would need.
// ---------------------------------------------------------------------

// connect_bootloader(), lines 152-635.
Status DensoSh705xEepromCanExecutor::connect_bootloader(ICanFlashTransport& transport, IClock& clock,
                                                        const ICancellationToken& cancellation, IEventSink& events,
                                                        const DensoSh705xEepromCanPlan& can_plan,
                                                        bool& kernel_alive) const
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before connect");
    }

    const std::uint32_t request_id = can_plan.request_id;

    events.log(LogLevel::Info, "Checking if kernel is already running...");
    Result<std::optional<bytes::Bytes>> probe = request_kernel_id(transport, clock, cancellation, request_id);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (probe->has_value() && looks_kernel_alive(**probe))
    {
        // lines 174-187: a well-formed alive response short-circuits the
        // rest of connect_bootloader() -- upload_kernel() is skipped too.
        kernel_alive = true;
        events.log(LogLevel::Info, "Kernel already running");
        return {};
    }
    // lines 174-191: BOTH "no frame at all" and "frame present but markers
    // wrong" fall through to the full init sequence below -- neither is a
    // hard failure at this point (same shape as the K-Line sibling's own
    // initial probe).
    events.log(LogLevel::Warning, "No response from kernel, initialising ECU...");

    // NOT IMPLEMENTED: the legacy "_ecutek_racerom_alt" branch (lines
    // 195-234) -- see the OPEN QUESTION resolution comment above this
    // function.

    events.log(LogLevel::Info, "Initializing connection...");
    if (Result<std::optional<bytes::Bytes>> init_resp =
            can_raw_exchange(transport, clock, cancellation, init_connection_request(request_id), kHandshakeDelayMs,
                             kHandshakeTimeoutMs);
        !init_resp.has_value())
    {
        return std::unexpected(init_resp.error());
    }
    // Response content is never inspected here -- legacy logs success/failure
    // but NEVER returns early on this step (Task 7 report, "Legacy behavior
    // surprises" #5). Faithfully reproduced, not a gap this task authorizes
    // closing: only a genuine transport-level failure (handled above)
    // propagates.

    events.log(LogLevel::Info, "Requesting ECU ID");
    if (Result<std::optional<bytes::Bytes>> ecuid_resp = can_raw_exchange(
            transport, clock, cancellation, ecu_id_request(request_id), kHandshakeDelayMs, kHandshakeTimeoutMs);
        !ecuid_resp.has_value())
    {
        return std::unexpected(ecuid_resp.error());
    }

    events.log(LogLevel::Info, "Requesting VIN");
    if (Result<std::optional<bytes::Bytes>> vin_resp = can_raw_exchange(
            transport, clock, cancellation, vin_request(request_id), kHandshakeDelayMs, kHandshakeTimeoutMs);
        !vin_resp.has_value())
    {
        return std::unexpected(vin_resp.error());
    }

    events.log(LogLevel::Info, "Requesting CAL ID");
    if (Result<std::optional<bytes::Bytes>> cal_resp = can_raw_exchange(
            transport, clock, cancellation, cal_id_request(request_id), kHandshakeDelayMs, kHandshakeTimeoutMs);
        !cal_resp.has_value())
    {
        return std::unexpected(cal_resp.error());
    }

    events.log(LogLevel::Info, "Requesting CVN");
    if (Result<std::optional<bytes::Bytes>> cvn_resp = can_raw_exchange(
            transport, clock, cancellation, cvn_request(request_id), kHandshakeDelayMs, kHandshakeTimeoutMs);
        !cvn_resp.has_value())
    {
        return std::unexpected(cvn_resp.error());
    }

    events.log(LogLevel::Info, "Requesting session mode");
    bool req_10_03_connected = false;
    Result<std::optional<bytes::Bytes>> s03_resp = can_raw_exchange(
        transport, clock, cancellation, session_mode_request(request_id, 0x03), kHandshakeDelayMs, kHandshakeTimeoutMs);
    if (!s03_resp.has_value())
    {
        return std::unexpected(s03_resp.error());
    }
    if (s03_resp->has_value() && (*s03_resp)->size() > 5 && (**s03_resp)[4] == 0x50 && (**s03_resp)[5] == 0x03)
    {
        req_10_03_connected = true;
    }

    bool req_10_43_connected = false;
    Result<std::optional<bytes::Bytes>> s43_resp = can_raw_exchange(
        transport, clock, cancellation, session_mode_request(request_id, 0x43), kHandshakeDelayMs, kHandshakeTimeoutMs);
    if (!s43_resp.has_value())
    {
        return std::unexpected(s43_resp.error());
    }
    if (s43_resp->has_value() && (*s43_resp)->size() > 5 && (**s43_resp)[4] == 0x50 && (**s43_resp)[5] == 0x43)
    {
        req_10_43_connected = true;
    }

    events.log(LogLevel::Info, "Requesting seed");
    Result<bytes::Bytes> seed_resp = can_exchange_gated(transport, clock, cancellation, seed_request(request_id),
                                                        kHandshakeDelayMs, kHandshakeTimeoutMs);
    if (!seed_resp.has_value())
    {
        return std::unexpected(seed_resp.error());
    }
    if (seed_resp->size() <= 5 || (*seed_resp)[4] != 0x67 || (*seed_resp)[5] != 0x01)
    {
        return fail(ErrorKind::BadResponse, "seed request rejected");
    }
    if (seed_resp->size() < 10)
    {
        return fail(ErrorKind::BadResponse, "seed response too short to contain a 4-byte seed");
    }
    const bytes::Bytes seed(seed_resp->begin() + 6, seed_resp->begin() + 10);

    // Dispatch over exactly the 4 values DensoSecurityVariant can hold -- no
    // default case (see the OPEN QUESTION resolution comment above).
    bytes::Bytes seed_key;
    switch (can_plan.security)
    {
    case DensoSecurityVariant::Stock:
        seed_key = generate_stock_seed_key(seed);
        break;
    case DensoSecurityVariant::EcuTek:
        seed_key = generate_ecutek_seed_key(seed);
        break;
    case DensoSecurityVariant::Cobb:
        seed_key = generate_cobb_seed_key(seed);
        break;
    case DensoSecurityVariant::EcuTekRaceRom:
        seed_key = generate_ecutek_racerom_can_seed_key(seed);
        break;
    }

    events.log(LogLevel::Info, "Sending seed key");
    Result<bytes::Bytes> key_resp =
        can_exchange_gated(transport, clock, cancellation, seed_key_send_request(request_id, seed_key),
                           kHandshakeDelayMs, kHandshakeTimeoutMs);
    if (!key_resp.has_value())
    {
        return std::unexpected(key_resp.error());
    }
    if (key_resp->size() <= 5 || (*key_resp)[4] != 0x67 || (*key_resp)[5] != 0x02)
    {
        return fail(ErrorKind::BadResponse, "seed key send rejected");
    }

    events.log(LogLevel::Info, "Set session mode");
    Result<bytes::Bytes> set_resp = can_exchange_gated(
        transport, clock, cancellation, session_set_request(request_id, req_10_03_connected, req_10_43_connected),
        kHandshakeDelayMs, kHandshakeTimeoutMs);
    if (!set_resp.has_value())
    {
        return std::unexpected(set_resp.error());
    }
    if (set_resp->size() <= 5 || (*set_resp)[4] != 0x50 || ((*set_resp)[5] != 0x02 && (*set_resp)[5] != 0x42))
    {
        return fail(ErrorKind::BadResponse, "session set rejected");
    }

    events.log(LogLevel::Info, "Successfully set to programming session");
    return {};
}

// upload_kernel(), lines 701-956.
Status DensoSh705xEepromCanExecutor::upload_kernel(ICanFlashTransport& transport, IClock& clock,
                                                   const ICancellationToken& cancellation, IEventSink& events,
                                                   const DensoSh705xEepromCanPlan& can_plan,
                                                   const KernelImage& kernel) const
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before kernel upload");
    }
    if (kernel.bytes.empty())
    {
        return fail(ErrorKind::InvalidConfig, "kernel image is empty");
    }

    const std::uint32_t request_id = can_plan.request_id;
    const std::uint32_t start_address = kernel.load_address;
    Result<DensoSh705xEepromUploadSizes> upload_sizes =
        denso_sh705x_eeprom_upload_sizes(FlashFamily::DensoSh705xEepromCan, kernel.bytes.size());
    if (!upload_sizes.has_value())
    {
        return std::unexpected(upload_sizes.error());
    }
    const std::uint32_t data_len = upload_sizes->payload_bytes;
    const std::uint32_t max_blocks = data_len / kUploadChunkBytes;

    // lines 734-760: pad the ORIGINAL file bytes up to data_len (always a
    // multiple of 128, hence of 4), drop the last 4 bytes, append a 32-bit
    // checksum word, then encrypt the whole data_len-byte buffer. Unlike the
    // K-Line sibling's upload_kernel(), buf.size() == len == data_len always
    // holds here by construction -- there is no latent OOB-read risk to fix
    // (Task 7 report: "there's no latent calculatePayload()-clamping/OOB-read
    // risk analogous to the K-Line finding").
    bytes::Bytes buf = kernel.bytes;
    buf.resize(data_len, 0);
    buf.resize(data_len - 4);
    std::uint32_t chk_sum = 0;
    for (std::size_t i = 0; i < buf.size(); i += 4)
    {
        chk_sum += (static_cast<std::uint32_t>(buf[i]) << 24) | (static_cast<std::uint32_t>(buf[i + 1]) << 16) |
                   (static_cast<std::uint32_t>(buf[i + 2]) << 8) | static_cast<std::uint32_t>(buf[i + 3]);
    }
    chk_sum = 0x5aa5a55aU - chk_sum;
    bytes::appendU32Be(buf, chk_sum);
    const bytes::Bytes encrypted = encrypt_can_kernel_payload(buf, static_cast<std::uint32_t>(buf.size()));

    events.log(LogLevel::Info, "Initialize kernel upload");
    Result<bytes::Bytes> download_resp =
        can_exchange_gated(transport, clock, cancellation, sid34_request(request_id, start_address, data_len),
                           kHandshakeDelayMs, kQuickAckTimeoutMs);
    if (!download_resp.has_value())
    {
        return std::unexpected(download_resp.error());
    }
    if (download_resp->size() <= 5 || (*download_resp)[4] != 0x74 || (*download_resp)[5] != 0x20)
    {
        return fail(ErrorKind::BadResponse, "kernel upload request rejected");
    }

    events.log(LogLevel::Info, "Uploading kernel, please wait...");
    const bytes::ByteView encrypted_view(encrypted);
    for (std::uint32_t blockno = 0; blockno <= max_blocks; ++blockno)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled between kernel transfer chunks");
        }

        const std::uint32_t block_addr = start_address + blockno * kUploadChunkBytes;
        // lines 816-857: the block loop runs blockno = 0..max_blocks
        // INCLUSIVE (max_blocks+1 iterations). Each of the first max_blocks
        // iterations sends a full 128-byte chunk and decrements a running
        // `data_len` copy by 128; since that running value starts at exactly
        // max_blocks*128, it has reached exactly 0 by the final
        // (blockno==max_blocks) iteration -- so the LAST 0xB6 frame this loop
        // sends is always header-only, zero payload bytes (Task 7 report,
        // "Legacy behavior surprises" #1). Faithfully reproduced, not
        // corrected: an N-block kernel produces N+1 wire frames.
        const bytes::ByteView chunk =
            blockno < max_blocks
                ? encrypted_view.subspan(static_cast<std::size_t>(blockno * kUploadChunkBytes), kUploadChunkBytes)
                : bytes::ByteView{};

        if (Status written = transport.write(sid_b6_request(request_id, block_addr, chunk), cancellation);
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled after kernel chunk write");
        }
        // No delay() call between this write and its read (legacy line
        // 852-853).
        if (auto block_resp = transport.read(kBlockAckTimeoutMs, cancellation); !block_resp.has_value())
        {
            return std::unexpected(block_resp.error());
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled after kernel chunk read");
        }
        // Response content is never inspected here (legacy line 853 just
        // discards it) -- reading it anyway keeps the wire exchange in
        // lockstep with a real ECU.

        events.progress(static_cast<int>(blockno), static_cast<int>(max_blocks));
    }

    events.log(LogLevel::Info, "Kernel uploaded, starting...");
    Result<bytes::Bytes> start_resp = can_exchange_gated(transport, clock, cancellation, sid37_request(request_id),
                                                         kHandshakeDelayMs, kQuickAckTimeoutMs);
    if (!start_resp.has_value())
    {
        return std::unexpected(start_resp.error());
    }
    if (start_resp->size() <= 4 || (*start_resp)[4] != 0x77)
    {
        return fail(ErrorKind::BadResponse, "kernel start ack rejected");
    }

    if (Status slept = clock.sleep(kPostUploadSettleDelayMs, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before start routine");
    }

    Result<bytes::Bytes> routine_resp = can_exchange_gated(transport, clock, cancellation, sid31_request(request_id),
                                                           kHandshakeDelayMs, kQuickAckTimeoutMs);
    if (!routine_resp.has_value())
    {
        return std::unexpected(routine_resp.error());
    }
    if (routine_resp->size() <= 4 || (*routine_resp)[4] != 0x71)
    {
        return fail(ErrorKind::BadResponse, "kernel start routine rejected");
    }

    events.log(LogLevel::Info, "Kernel requesting kernel ID...");
    // Unlike the K-Line sibling's up-to-10-iteration poll loop, this is a
    // SINGLE attempt -- legacy lines 930-953 `return STATUS_ERROR`
    // immediately on a non-alive response here, with no retry.
    Result<std::optional<bytes::Bytes>> poll = request_kernel_id(transport, clock, cancellation, request_id);
    if (!poll.has_value())
    {
        return std::unexpected(poll.error());
    }
    if (!poll->has_value())
    {
        return fail(ErrorKind::Timeout, "kernel did not respond after upload");
    }
    if (!looks_kernel_alive(**poll))
    {
        return fail(ErrorKind::BadResponse, "kernel did not report a valid ID after upload");
    }

    events.log(LogLevel::Info, "Kernel is alive");
    return {};
}

// read_mem(), lines 963-1139 ("BEEF/READ_EEPROM" combined-request protocol).
Result<bytes::Bytes> DensoSh705xEepromCanExecutor::read_mem(ICanFlashTransport& transport, IClock& clock,
                                                            const ICancellationToken& cancellation, IEventSink& events,
                                                            const MemoryRegion& region, EepromReadMode mode,
                                                            std::uint32_t request_id) const
{
    const std::uint32_t start_addr = region.start;
    const std::uint32_t length = region.length;

    std::uint32_t pagesize = kMaxEepromPageBytes;
    if (pagesize > length)
    {
        pagesize = length;
    }

    std::uint32_t skip_start = start_addr & (pagesize - 1);
    std::uint32_t addr = start_addr - skip_start;
    std::uint32_t willget = (skip_start + length + pagesize - 1) & ~(pagesize - 1);
    std::uint32_t len_done = 0;

    bytes::Bytes mapdata;
    events.progress(0, static_cast<int>(length));

    while (willget != 0)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during EEPROM read");
        }

        constexpr std::uint32_t kNumBlocks = 1; // legacy hardcodes this per outer iteration

        if (Status written = transport.write(
                read_eeprom_request(request_id, static_cast<std::uint8_t>(mode), addr, pagesize), cancellation);
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled after EEPROM read request");
        }

        // No delay() call between this write and the header-ack read (legacy
        // line 1036 has it commented out).
        auto header = transport.read(kPageHeaderTimeoutMs, cancellation);
        if (!header.has_value())
        {
            return std::unexpected(header.error());
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled after EEPROM header ack");
        }
        if (!header->has_value())
        {
            return fail(ErrorKind::Timeout, "no response to EEPROM read request");
        }
        const bytes::Bytes& hdr = **header;
        if (hdr.size() <= 8)
        {
            return fail(ErrorKind::BadResponse, "EEPROM read header ack too short");
        }
        if (hdr[4] == 0xBE && hdr[5] == 0xEF && hdr[8] == static_cast<bytes::Byte>(kSubKernelReadArea | 0x40))
        {
            mapdata.insert(mapdata.end(), hdr.begin() + 9, hdr.end());
        }
        // else: a well-formed-length-but-wrong-content header ack is
        // silently swallowed here -- neither an error nor an append (Task 7
        // report, "Legacy behavior surprises" #3). Faithfully reproduced.

        bytes::Bytes pagedata;
        int attempts = 0;
        while (pagedata.size() < pagesize && attempts < kMaxPagedataAttempts)
        {
            if (cancellation.cancelled())
            {
                return fail(ErrorKind::Cancelled, "cancelled during EEPROM page read");
            }
            auto chunk = transport.read(kPagedataPollTimeoutMs, cancellation);
            if (!chunk.has_value())
            {
                return std::unexpected(chunk.error());
            }
            if (cancellation.cancelled())
            {
                return fail(ErrorKind::Cancelled, "cancelled during EEPROM page read");
            }
            if (chunk->has_value() && !(*chunk)->empty())
            {
                pagedata.insert(pagedata.end(), (*chunk)->begin(), (*chunk)->end());
            }
            ++attempts;
        }
        // line 1069: `if (timeout >= 1000)` is legacy dead code -- the loop's
        // own bound is kMaxPagedataAttempts (100), so this can never trip. A
        // page that never fully arrives is therefore silently accepted short
        // rather than erroring; preserved faithfully (Task 7 report, "Legacy
        // behavior surprises" #2), same shape as the K-Line sibling's own
        // analogous dead-code note.

        if (pagedata.size() > 7)
        {
            pagedata.erase(pagedata.begin(), pagedata.begin() + 8);
        }
        // Unlike the K-Line sibling, mapdata is never sliced by cplen/
        // skip_start here -- it just accumulates whatever survived each
        // response's own framing strip, whole (Task 7 report).
        mapdata.insert(mapdata.end(), pagedata.begin(), pagedata.end());

        std::uint32_t cplen = kNumBlocks * pagesize - skip_start;
        skip_start = 0;

        if (Status slept = clock.sleep(kInterPageDelayMs, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }

        if (const std::uint32_t extrabytes = cplen + len_done; extrabytes > length)
        {
            cplen -= (extrabytes - length);
        }
        len_done += cplen;
        addr += kNumBlocks * pagesize;
        willget -= kNumBlocks * pagesize;

        events.progress(static_cast<int>(len_done), static_cast<int>(length));
    }

    events.progress(static_cast<int>(length), static_cast<int>(length));
    return mapdata;
}

} // namespace fastecu::flash
