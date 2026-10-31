#include "src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_types.h"

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/ports/cancellation.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <array>

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------
// Literal protocol constants transcribed from
// src/backend/flash/kernel/kernelcomms.h (not #included: the K-Line sibling
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
constexpr int kHandshakeDelayMs = 50;                          // delay(50) before every connect_bootloader() read
constexpr std::chrono::milliseconds kHandshakeTimeout{2000};   // serial_read_timeout
constexpr int kKernelIdRequestDelayMs = 200;                   // request_kernel_id() delay(200)
constexpr std::chrono::milliseconds kKernelIdReadTimeout{800}; // serial_read_long_timeout
constexpr std::chrono::milliseconds kQuickAckTimeout{10}; // literal "10" -- upload_kernel()'s SID34/0x37/0x31 ack reads
constexpr std::chrono::milliseconds kBlockAckTimeout{500}; // receive_timeout -- 0xB6 block acks (no delay before)
constexpr int kPostUploadSettleDelayMs = 100;              // upload_kernel() delay(100)
constexpr std::chrono::milliseconds kPageHeaderTimeout{
    2000}; // serial_read_timeout -- read_mem()'s header-ack read (no delay before)
constexpr std::chrono::milliseconds kPagedataPollTimeout{200}; // serial_read_short_timeout
constexpr int kMaxPagedataAttempts = 100; // read_mem()'s inner accumulation loop cap (`timeout < 100`)
constexpr int kInterPageDelayMs = 1;      // read_mem() delay(1)

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

bytes::Bytes CanFrame(std::uint32_t request_id, bytes::ByteView payload)
{
    return ComposeBe(request_id, payload);
}

bytes::Bytes InitConnectionRequest(std::uint32_t request_id)
{
    return CanFrame(request_id, bytes::Bytes{0x01, 0x00});
}
bytes::Bytes EcuIdRequest(std::uint32_t request_id)
{
    return CanFrame(request_id, bytes::Bytes{0xAA});
}
bytes::Bytes VinRequest(std::uint32_t request_id)
{
    return CanFrame(request_id, bytes::Bytes{0x09, 0x02});
}
bytes::Bytes CalIdRequest(std::uint32_t request_id)
{
    return CanFrame(request_id, bytes::Bytes{0x09, 0x04});
}
bytes::Bytes CvnRequest(std::uint32_t request_id)
{
    return CanFrame(request_id, bytes::Bytes{0x09, 0x06});
}
bytes::Bytes SessionModeRequest(std::uint32_t request_id, std::uint8_t mode)
{
    return CanFrame(request_id, bytes::Bytes{0x10, static_cast<bytes::Byte>(mode)});
}
bytes::Bytes SeedRequest(std::uint32_t request_id)
{
    return CanFrame(request_id, bytes::Bytes{0x27, 0x01});
}
bytes::Bytes SeedKeySendRequest(std::uint32_t request_id, bytes::ByteView key)
{
    return CanFrame(request_id, ComposeBe(0x27_b, 0x02_b, key));
}
// [0x10] with 0x02 appended iff req_10_03_connected, then 0x42 appended iff
// req_10_43_connected. Computed dynamically, not hardcoded to "both true" --
// the two prior session-mode checks never hard-fail on mismatch, so
// either flag can legitimately end up false.
bytes::Bytes SessionSetRequest(std::uint32_t request_id, bool req_10_03_connected, bool req_10_43_connected)
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
    return CanFrame(request_id, payload);
}
// UNLIKE the K-Line sibling's request_kernel_id(), this one has NO trailing
// checksum byte.
bytes::Bytes RequestKernelIdFrame(std::uint32_t request_id)
{
    // Trailing 3 zero bytes (0x00,0x00,0x00) are NOT checksum -- see the
    // comment above this function -- they're literal payload padding present
    // in the legacy frame; composeBe can't infer them from kSubKernelId's
    // type, so they're spelled out explicitly to keep the 8-byte payload
    // length exact.
    return CanFrame(request_id, ComposeBe(kSubKernelStartComm, std::uint16_t{1}, kSubKernelId, 0x00_b, 0x00_b, 0x00_b));
}
bytes::Bytes Sid34Request(std::uint32_t request_id, std::uint32_t addr, std::uint32_t data_len)
{
    return CanFrame(request_id, ComposeBe(0x34_b, 0x04_b, 0x33_b, U24{addr}, U24{data_len}));
}
bytes::Bytes SidB6Request(std::uint32_t request_id, std::uint32_t block_addr, bytes::ByteView chunk)
{
    return CanFrame(request_id, ComposeBe(0xB6_b, U24{block_addr}, chunk));
}
bytes::Bytes Sid37Request(std::uint32_t request_id)
{
    return CanFrame(request_id, bytes::Bytes{0x37});
}
bytes::Bytes Sid31Request(std::uint32_t request_id)
{
    return CanFrame(request_id, bytes::Bytes{0x31, 0x01, 0x02, 0x02, 0x02});
}
// read_mem(): datalen==6 is hardcoded in the request template and again on
// every iteration.
bytes::Bytes ReadEepromRequest(std::uint32_t request_id, std::uint8_t mode, std::uint32_t addr, std::uint32_t pagesize)
{
    constexpr std::uint32_t kDatalen = 6;
    return CanFrame(request_id, ComposeBe(kSubKernelStartComm, std::uint16_t(kDatalen + 1), kSubKernelReadEeprom, mode,
                                          U24{addr}, std::uint16_t(pagesize)));
}

// request_kernel_id()'s / upload_kernel()'s post-upload poll's "kernel alive"
// marker check: received[4..5] == SUB_KERNEL_START_COMM big-endian, received[8]
// == SUB_KERNEL_ID | 0x40.
bool LooksKernelAlive(bytes::ByteView received)
{
    return received.size() > 8 && bytes::ReadU16Be(received, 4) == kSubKernelStartComm &&
           received[8] == static_cast<bytes::Byte>(kSubKernelId | 0x40U);
}

// ---------------------------------------------------------------------
// Seed-key algorithms -- transcribed from each generate_*_seed_key() body.
// This class's tables are its OWN, distinct from the K-Line sibling's
// (Task 7 report, "Legacy behavior surprises" #8) -- verified against
// task-7-report.md's independently re-transcribed tables, not copy-pasted
// from the K-Line executor.
// ---------------------------------------------------------------------

// generate_seed_key(), (Stock).
bytes::Bytes GenerateStockSeedKey(bytes::ByteView seed)
{
    static constexpr std::array<std::uint16_t, 16> kIndex{0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4,
                                                          0xFD21, 0x5E71, 0xB046, 0x7F4A, 0x4B75, 0x93F9,
                                                          0x1895, 0x8961, 0x3ECC, 0x862B};
    return ssm_protocol::CalculateSeedKey(seed, kIndex, ssm_protocol::kStockPairTable);
}

// generate_ecutek_seed_key(), (base calculateSeedKey() call only, -- the
// "_ecutek_racerom_alt" post-processing is intentionally not
// reproduced; see the OPEN QUESTION resolution comment above
// connect_bootloader() below). The same keytogenerateindex_1 as Stock, paired
// with the ECUTEK index transformation.
bytes::Bytes GenerateEcutekSeedKey(bytes::ByteView seed)
{
    static constexpr std::array<std::uint16_t, 16> kIndex{0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4,
                                                          0xFD21, 0x5E71, 0xB046, 0x7F4A, 0x4B75, 0x93F9,
                                                          0x1895, 0x8961, 0x3ECC, 0x862B};
    return ssm_protocol::CalculateSeedKey(seed, kIndex, ssm_protocol::kEcutekPairTable);
}

// generate_cobb_seed_key(), ("2017 VA model" table -- the one actually passed
// to calculateSeedKey()).
bytes::Bytes GenerateCobbSeedKey(bytes::ByteView seed)
{
    static constexpr std::array<std::uint16_t, 16> kIndex{0x9DDB, 0x9CFB, 0x9B9A, 0x6136, 0x59E1, 0xBA03,
                                                          0xD683, 0x7092, 0x9E05, 0x8723, 0xF998, 0x15BB,
                                                          0xB8D5, 0xFF0C, 0x9D91, 0x24B9};
    return ssm_protocol::CalculateSeedKey(seed, kIndex, ssm_protocol::kStockPairTable);
}

// Plain modular exponentiation (square-and-multiply) -- no SsmProtocol
// equivalent, replicated verbatim.
std::uint64_t DecryptRaceromSeed(std::uint64_t base, std::uint64_t exponent, std::uint64_t modulus)
{
    std::uint64_t result = 1;
    base = base % modulus;
    while (exponent > 0)
    {
        if (exponent & 1U)
        {
            result = (result * base) % modulus;
        }
        base = (base * base) % modulus;
        exponent /= 2;
    }
    return result;
}

// Seed packed big-endian into a uint32, RSA-decrypted with hardcoded d/n,
// re-emitted big-endian as the 4-byte key.
bytes::Bytes GenerateEcutekRaceromCanSeedKey(bytes::ByteView seed)
{
    const std::uint32_t seed_word = bytes::ReadU32Be(seed, 0);
    constexpr std::uint64_t kD = 0x0A863281ULL;
    constexpr std::uint64_t kN = 0x0fda9293ULL;
    const std::uint32_t decrypted = static_cast<std::uint32_t>(DecryptRaceromSeed(seed_word, kD, kN));
    return bytes::ComposeBeWithExtraCapacity(0, decrypted);
}

// This class's OWN key table -- {0xC85B, 0x32C0, 0xE282, 0x92A0} -- distinct
// from the K-Line sibling's ({0x7856, 0xCE22, 0xF513, 0x6E86}).
bytes::Bytes EncryptCanKernelPayload(bytes::ByteView buf)
{
    static constexpr std::array<std::uint16_t, 4> kIndex{0xC85B, 0x32C0, 0xE282, 0x92A0};
    return ssm_protocol::CalculatePayload(buf, kIndex, ssm_protocol::kStockPairTable);
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
Result<std::optional<bytes::Bytes>> CanRawExchange(ICanFlashTransport& transport, IClock& clock,
                                                   const ICancellationToken& cancellation, bytes::ByteView request,
                                                   int delay_ms, std::chrono::milliseconds timeout)
{
    return CheckCancelled(cancellation, "cancelled before write")
        .and_then([&] { return transport.Write(request, cancellation); })
        .and_then([&] { return CheckCancelled(cancellation, "cancelled after write"); })
        // Every caller passes a positive delay.
        .and_then([&] { return clock.Sleep(std::chrono::milliseconds{delay_ms}, cancellation); })
        .and_then([&] { return CheckCancelled(cancellation, "cancelled after delay"); })
        .and_then([&] { return transport.Read(timeout, cancellation); })
        .and_then(
            [&](std::optional<bytes::Bytes> received)
            {
                return CheckCancelled(cancellation, "cancelled after read")
                    .transform([&] { return std::move(received); });
            });
}

// Gated variant: an absent response maps to ErrorKind::kTimeout. Used only at
// connect_bootloader()'s 3 hard-gating steps (seed request, seed-key send,
// session set) and upload_kernel()'s SID34/0x37/0x31 acks -- exactly the
// sites where legacy's own `if (received.length() > N) {...} else { return
// STATUS_ERROR; }` shape means a short-or-absent response is a hard failure.
Result<bytes::Bytes> CanExchangeGated(ICanFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation, bytes::ByteView request, int delay_ms,
                                      std::chrono::milliseconds timeout)
{
    Result<std::optional<bytes::Bytes>> raw =
        CanRawExchange(transport, clock, cancellation, request, delay_ms, timeout);
    if (!raw.has_value())
    {
        return std::unexpected(raw.error());
    }
    if (!raw->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response from ECU");
    }
    return std::move(**raw);
}

// request_kernel_id(): a write, then delay(200) and
// read(serial_read_long_timeout). An empty/absent response here is NOT an
// error -- it's the normal "kernel not (yet) running" signal that both
// connect_bootloader()'s initial probe and upload_kernel()'s single post-upload
// poll interpret themselves.
Result<std::optional<bytes::Bytes>> RequestKernelId(ICanFlashTransport& transport, IClock& clock,
                                                    const ICancellationToken& cancellation, std::uint32_t request_id)
{
    return CanRawExchange(transport, clock, cancellation, RequestKernelIdFrame(request_id), kKernelIdRequestDelayMs,
                          kKernelIdReadTimeout);
}

// Legacy's positive-response check: the service at byte 4, its echo at 5.
bool RepliesWith(bytes::ByteView response, bytes::Byte service, bytes::Byte echo)
{
    return response.size() > 5 && response[4] == service && response[5] == echo;
}

// The ECU/CAL identity queries: legacy logs their outcome but never returns
// early on content (Task 7 report, "Legacy behavior surprises" #5), so only a
// genuine transport-level failure propagates.
Status QueryIdentity(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                     IEventSink& events, std::uint32_t request_id)
{
    for (const auto& [label, request] :
         {std::pair{"Initializing connection...", InitConnectionRequest(request_id)},
          std::pair{"Requesting ECU ID", EcuIdRequest(request_id)}, std::pair{"Requesting VIN", VinRequest(request_id)},
          std::pair{"Requesting CAL ID", CalIdRequest(request_id)},
          std::pair{"Requesting CVN", CvnRequest(request_id)}})
    {
        events.Log(LogLevel::kInfo, label);
        if (auto response =
                CanRawExchange(transport, clock, cancellation, request, kHandshakeDelayMs, kHandshakeTimeout);
            !response.has_value())
        {
            return std::unexpected(response.error());
        }
    }
    return {};
}

// True when the ECU accepted diagnostic session `mode`.
Result<bool> RequestSession(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                            std::uint32_t request_id, std::uint8_t mode)
{
    Result<std::optional<bytes::Bytes>> response = CanRawExchange(
        transport, clock, cancellation, SessionModeRequest(request_id, mode), kHandshakeDelayMs, kHandshakeTimeout);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    return response->has_value() && RepliesWith(**response, 0x50, mode);
}

// Dispatch over exactly the 4 values DensoSecurityVariant can hold -- no
// default case (see the OPEN QUESTION resolution comment above).
bytes::Bytes SeedKey(DensoSecurityVariant security, bytes::ByteView seed)
{
    switch (security)
    {
    case DensoSecurityVariant::kStock:
        return GenerateStockSeedKey(seed);
    case DensoSecurityVariant::kEcuTek:
        return GenerateEcutekSeedKey(seed);
    case DensoSecurityVariant::kCobb:
        return GenerateCobbSeedKey(seed);
    case DensoSecurityVariant::kEcuTekRaceRom:
        return GenerateEcutekRaceromCanSeedKey(seed);
    }
    return {};
}

Status UnlockSecurity(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                      IEventSink& events, const DensoSh705xEepromCanPlan& can_plan)
{
    events.Log(LogLevel::kInfo, "Requesting seed");
    return CanExchangeGated(transport, clock, cancellation, SeedRequest(can_plan.request_id), kHandshakeDelayMs,
                            kHandshakeTimeout)
        .and_then(
            [&](const bytes::Bytes& seed_resp) -> Result<bytes::Bytes>
            {
                if (!RepliesWith(seed_resp, 0x67, 0x01))
                {
                    return Fail(ErrorKind::kBadResponse, "seed request rejected");
                }
                if (seed_resp.size() < 10)
                {
                    return Fail(ErrorKind::kBadResponse, "seed response too short to contain a 4-byte seed");
                }
                const bytes::Bytes seed_key = SeedKey(can_plan.security, bytes::ByteView(seed_resp).subspan(6, 4));
                events.Log(LogLevel::kInfo, "Sending seed key");
                return CanExchangeGated(transport, clock, cancellation,
                                        SeedKeySendRequest(can_plan.request_id, seed_key), kHandshakeDelayMs,
                                        kHandshakeTimeout);
            })
        .and_then(
            [&](const bytes::Bytes& key_resp) -> Status
            {
                if (!RepliesWith(key_resp, 0x67, 0x02))
                {
                    return Fail(ErrorKind::kBadResponse, "seed key send rejected");
                }
                return {};
            });
}

// Pad the ORIGINAL file bytes up to data_len (always a multiple of 128, hence
// of 4), drop the last 4 bytes, append a 32-bit checksum word, then encrypt the
// whole data_len-byte buffer. Unlike the K-Line sibling's upload_kernel(),
// buf.size() == len == data_len always holds here by construction -- there is
// no latent OOB-read risk to fix (Task 7 report: "there's no latent
// calculatePayload()-clamping/OOB-read risk analogous to the K-Line finding").
bytes::Bytes KernelPayload(bytes::ByteView kernel, std::uint32_t data_len)
{
    bytes::Bytes buf(kernel.begin(), kernel.end());
    buf.resize(data_len - 4, 0);
    std::uint32_t chk_sum = 0;
    for (std::size_t i = 0; i < buf.size(); i += 4)
    {
        chk_sum += bytes::ReadU32Be(buf, i);
    }
    bytes::AppendU32Be(buf, 0x5aa5a55aU - chk_sum);
    return EncryptCanKernelPayload(buf);
}

// The block loop runs blockno = 0..max_blocks INCLUSIVE (max_blocks+1
// iterations). Each of the first max_blocks iterations sends a full 128-byte
// chunk and decrements a running `data_len` copy by 128; since that running
// value starts at exactly max_blocks*128, it has reached exactly 0 by the final
// (blockno==max_blocks) iteration -- so the LAST 0xB6 frame this loop sends is
// always header-only, zero payload bytes (Task 7 report, "Legacy behavior
// surprises" #1). Faithfully reproduced, not corrected: an N-block kernel
// produces N+1 wire frames.
Status SendKernelBlocks(ICanFlashTransport& transport, const ICancellationToken& cancellation, IEventSink& events,
                        std::uint32_t request_id, std::uint32_t start_address, bytes::ByteView encrypted)
{
    const auto max_blocks = static_cast<std::uint32_t>(encrypted.size() / kUploadChunkBytes);
    for (std::uint32_t blockno = 0; blockno <= max_blocks; ++blockno)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled between kernel transfer chunks");
        }
        const std::uint32_t block_addr = start_address + blockno * kUploadChunkBytes;
        const bytes::ByteView chunk =
            blockno < max_blocks
                ? encrypted.subspan(static_cast<std::size_t>(blockno) * kUploadChunkBytes, kUploadChunkBytes)
                : bytes::ByteView{};
        if (Status written = transport.Write(SidB6Request(request_id, block_addr, chunk), cancellation);
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled after kernel chunk write");
        }
        // No delay() call between this write and its read. Response content is
        // never inspected (legacy just discards it) -- reading it anyway keeps
        // the wire exchange in lockstep with a real ECU.
        if (auto block_resp = transport.Read(kBlockAckTimeout, cancellation); !block_resp.has_value())
        {
            return std::unexpected(block_resp.error());
        }
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled after kernel chunk read");
        }
        events.Progress(static_cast<int>(blockno), static_cast<int>(max_blocks));
    }
    return {};
}

Status StartKernel(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                   IEventSink& events, std::uint32_t request_id)
{
    events.Log(LogLevel::kInfo, "Kernel uploaded, starting...");
    return CanExchangeGated(transport, clock, cancellation, Sid37Request(request_id), kHandshakeDelayMs,
                            kQuickAckTimeout)
        .and_then(
            [&](const bytes::Bytes& start_resp) -> Status
            {
                if (start_resp.size() < 5 || start_resp[4] != 0x77)
                {
                    return Fail(ErrorKind::kBadResponse, "kernel start ack rejected");
                }
                return clock.Sleep(std::chrono::milliseconds{kPostUploadSettleDelayMs}, cancellation);
            })
        .and_then([&] { return CheckCancelled(cancellation, "cancelled before start routine"); })
        .and_then(
            [&]
            {
                return CanExchangeGated(transport, clock, cancellation, Sid31Request(request_id), kHandshakeDelayMs,
                                        kQuickAckTimeout);
            })
        .and_then(
            [&](const bytes::Bytes& routine_resp) -> Status
            {
                if (routine_resp.size() < 5 || routine_resp[4] != 0x71)
                {
                    return Fail(ErrorKind::kBadResponse, "kernel start routine rejected");
                }
                return {};
            });
}

// The page body that follows a header ack: up to kMaxPagedataAttempts polls.
// `if (timeout >= 1000)` is legacy dead code -- the loop's own bound is
// kMaxPagedataAttempts (100), so this can never trip. A page that never fully
// arrives is therefore silently accepted short rather than erroring; preserved
// faithfully (Task 7 report, "Legacy behavior surprises" #2), same shape as the
// K-Line sibling's own analogous dead-code note.
Result<bytes::Bytes> ReadPageData(ICanFlashTransport& transport, const ICancellationToken& cancellation,
                                  std::uint32_t pagesize)
{
    bytes::Bytes pagedata;
    for (int attempts = 0; pagedata.size() < pagesize && attempts < kMaxPagedataAttempts; ++attempts)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during EEPROM page read");
        }
        auto chunk = transport.Read(kPagedataPollTimeout, cancellation);
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during EEPROM page read");
        }
        if (chunk->has_value())
        {
            pagedata.append_range(**chunk);
        }
    }
    return pagedata;
}

// One combined BEEF/READ_EEPROM request, returning the bytes it contributes:
// the header ack's payload, then the page body without its 8-byte framing.
Result<bytes::Bytes> ReadEepromPage(ICanFlashTransport& transport, const ICancellationToken& cancellation,
                                    std::uint32_t request_id, EepromReadMode mode, std::uint32_t addr,
                                    std::uint32_t pagesize)
{
    return transport.Write(ReadEepromRequest(request_id, static_cast<std::uint8_t>(mode), addr, pagesize), cancellation)
        .and_then([&] { return CheckCancelled(cancellation, "cancelled after EEPROM read request"); })
        // No delay() call between this write and the header-ack read (legacy
        // has it commented out).
        .and_then([&] { return transport.Read(kPageHeaderTimeout, cancellation); })
        .and_then(
            [&](const std::optional<bytes::Bytes>& header) -> Result<bytes::Bytes>
            {
                if (cancellation.Cancelled())
                {
                    return Fail(ErrorKind::kCancelled, "cancelled after EEPROM header ack");
                }
                if (!header.has_value())
                {
                    return Fail(ErrorKind::kTimeout, "no response to EEPROM read request");
                }
                const bytes::Bytes& hdr = *header;
                if (hdr.size() < 9)
                {
                    return Fail(ErrorKind::kBadResponse, "EEPROM read header ack too short");
                }
                bytes::Bytes contributed;
                // A well-formed-length-but-wrong-content header ack is silently
                // swallowed -- neither an error nor an append (Task 7 report, "Legacy
                // behavior surprises" #3). Faithfully reproduced.
                if (hdr[4] == 0xBE && hdr[5] == 0xEF && hdr[8] == static_cast<bytes::Byte>(kSubKernelReadArea | 0x40U))
                {
                    contributed.assign(hdr.begin() + 9, hdr.end());
                }
                return ReadPageData(transport, cancellation, pagesize)
                    .transform(
                        [&](const bytes::Bytes& pagedata)
                        {
                            if (pagedata.size() >= 8)
                            {
                                contributed.append_range(bytes::ByteView(pagedata).subspan(8));
                            }
                            else
                            {
                                contributed.append_range(pagedata);
                            }
                            return std::move(contributed);
                        });
            });
}

} // namespace

Result<Iso15765Config> DensoSh705xEepromCanExecutor::Setup(const DensoSh705xEepromCanPlan& can_plan) const
{
    return Iso15765ConfigFrom(can_plan);
}

Result<FlashExecutionResult> DensoSh705xEepromCanExecutor::Run(const DensoSh705xEepromCanPlan& can_plan,
                                                               const FlashPlan& plan, ICanFlashTransport& transport,
                                                               IClock& clock, const ICancellationToken& cancellation,
                                                               IEventSink& events)
{
    ICanFlashTransport& can_transport = transport;
    bool kernel_alive = false;
    return ConnectBootloader(can_transport, clock, cancellation, events, can_plan, kernel_alive)
        .and_then(
            [&]() -> Status
            {
                if (kernel_alive)
                {
                    return {};
                }
                // Safe to dereference: this executor only ever runs against
                // DensoSh705xEepromCan plans, and validate_and_build rejects any plan
                // for that family whose kernel is absent (flash_validation.cpp:
                // "family requires a kernel image"). check_family above confirms
                // the family; it does not itself
                // guarantee a kernel -- validate_and_build is what does.
                return UploadKernel(can_transport, clock, cancellation, events, can_plan, plan.KernelOrEmpty());
            })
        .and_then(
            [&]
            {
                return ReadMem(can_transport, clock, cancellation, events, plan.TransferRegion(), can_plan.mode,
                               can_plan.request_id);
            })
        .transform(
            [&](bytes::Bytes read_bytes)
            {
                return FlashExecutionResult{
                    .operation = plan.Operation(),
                    .read_bytes = std::move(read_bytes),
                };
            });
}

// ---------------------------------------------------------------------
// Resolution of Task 4's OPEN QUESTION (denso_sh705x_eeprom_common.cpp): the
// legacy "_ecutek_racerom_alt" flash-method branch (connect_bootloader()) is
// intentionally NOT implemented below. Full reasoning is in task-9-report.md;
// summary:
//
//   1. DensoSecurityVariant (fixed at 4 values by Task 4, before this task)
//      has no representation for "_ecutek_racerom_alt" -- and adding a 5th
//      value or a separate plan field would only be meaningful once this
//      executor could actually perform the branch's prerequisite step: a
//      temporary K-Line-shaped SSM exchange (read_ram_location(), addHeader-
//      framed, no CAN-ID prefix) multiplexed over the SAME physical adapter
//      the plan declares as TransportKind::kCanIso15765. Nothing in the
//      portable transport seam (ICanFlashTransport/Iso15765Config) exposes a
//      "become K-Line for a moment" capability; inventing one is a
//      transport-architecture change well beyond this task's scope (build
//      the CAN executor), not a per-security-variant branch.
//   2. Even setting (1) aside, Task 7's characterization work could only pin
//      read_ram_location()'s FAILURE path (a short/malformed response taking
//      its early `return STATUS_ERROR` branch) -- the "success" path reads its
//      result from `response`, a QByteArray that is never populated anywhere in
//      the function (the real bytes land in `received`), so on real hardware a
//      well-formed ACK there is undefined behavior (an out-of-bounds QByteArray
//      read), not a stable, reproducible contract. There is no golden trace for
//      what the resulting seed key should be in that case, so implementing that
//      pipeline now would mean inventing unverified behavior -- exactly what
//      Task 7 declined to do, and exactly what the design spec's
//      compatibility-contract rule forbids guessing at.
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

Status DensoSh705xEepromCanExecutor::ConnectBootloader(ICanFlashTransport& transport, IClock& clock,
                                                       const ICancellationToken& cancellation, IEventSink& events,
                                                       const DensoSh705xEepromCanPlan& can_plan,
                                                       bool& kernel_alive) const
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before connect");
    }

    const std::uint32_t request_id = can_plan.request_id;

    events.Log(LogLevel::kInfo, "Checking if kernel is already running...");
    Result<std::optional<bytes::Bytes>> probe = RequestKernelId(transport, clock, cancellation, request_id);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (probe->has_value() && LooksKernelAlive(**probe))
    {
        // A well-formed alive response short-circuits the rest of
        // connect_bootloader() -- upload_kernel() is skipped too.
        kernel_alive = true;
        events.Log(LogLevel::kInfo, "Kernel already running");
        return {};
    }
    // BOTH "no frame at all" and "frame present but markers wrong" fall through
    // to the full init sequence below -- neither is a hard failure at this
    // point (same shape as the K-Line sibling's own initial probe).
    events.Log(LogLevel::kWarning, "No response from kernel, initialising ECU...");

    // NOT IMPLEMENTED: the legacy "_ecutek_racerom_alt" branch -- see the OPEN
    // QUESTION resolution comment above this function.

    if (Status identity = QueryIdentity(transport, clock, cancellation, events, request_id); !identity.has_value())
    {
        return identity;
    }
    events.Log(LogLevel::kInfo, "Requesting session mode");
    Result<bool> req_10_03_connected = RequestSession(transport, clock, cancellation, request_id, 0x03);
    if (!req_10_03_connected.has_value())
    {
        return std::unexpected(req_10_03_connected.error());
    }
    Result<bool> req_10_43_connected = RequestSession(transport, clock, cancellation, request_id, 0x43);
    if (!req_10_43_connected.has_value())
    {
        return std::unexpected(req_10_43_connected.error());
    }
    if (Status unlocked = UnlockSecurity(transport, clock, cancellation, events, can_plan); !unlocked.has_value())
    {
        return unlocked;
    }

    events.Log(LogLevel::kInfo, "Set session mode");
    Result<bytes::Bytes> set_resp = CanExchangeGated(
        transport, clock, cancellation, SessionSetRequest(request_id, *req_10_03_connected, *req_10_43_connected),
        kHandshakeDelayMs, kHandshakeTimeout);
    if (!set_resp.has_value())
    {
        return std::unexpected(set_resp.error());
    }
    if (!RepliesWith(*set_resp, 0x50, 0x02) && !RepliesWith(*set_resp, 0x50, 0x42))
    {
        return Fail(ErrorKind::kBadResponse, "session set rejected");
    }

    events.Log(LogLevel::kInfo, "Successfully set to programming session");
    return {};
}

Status DensoSh705xEepromCanExecutor::UploadKernel(ICanFlashTransport& transport, IClock& clock,
                                                  const ICancellationToken& cancellation, IEventSink& events,
                                                  const DensoSh705xEepromCanPlan& can_plan,
                                                  const KernelImage& kernel) const
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before kernel upload");
    }
    if (kernel.bytes.empty())
    {
        return Fail(ErrorKind::kInvalidConfig, "kernel image is empty");
    }

    const std::uint32_t request_id = can_plan.request_id;
    const std::uint32_t start_address = kernel.load_address;
    Result<DensoSh705xEepromUploadSizes> upload_sizes =
        ComputeDensoSh705xEepromUploadSizes(FlashFamily::kDensoSh705xEepromCan, kernel.bytes.size());
    if (!upload_sizes.has_value())
    {
        return std::unexpected(upload_sizes.error());
    }
    const std::uint32_t data_len = upload_sizes->payload_bytes;
    const bytes::Bytes encrypted = KernelPayload(kernel.bytes, data_len);

    events.Log(LogLevel::kInfo, "Initialize kernel upload");
    return CanExchangeGated(transport, clock, cancellation, Sid34Request(request_id, start_address, data_len),
                            kHandshakeDelayMs, kQuickAckTimeout)
        .and_then(
            [&](const bytes::Bytes& download_resp) -> Status
            {
                if (!RepliesWith(download_resp, 0x74, 0x20))
                {
                    return Fail(ErrorKind::kBadResponse, "kernel upload request rejected");
                }
                events.Log(LogLevel::kInfo, "Uploading kernel, please wait...");
                return SendKernelBlocks(transport, cancellation, events, request_id, start_address, encrypted);
            })
        .and_then([&] { return StartKernel(transport, clock, cancellation, events, request_id); })
        .and_then(
            [&]
            {
                events.Log(LogLevel::kInfo, "Kernel requesting kernel ID...");
                // Unlike the K-Line sibling's up-to-10-iteration poll loop, this is a
                // SINGLE attempt -- legacy `return STATUS_ERROR` immediately on a
                // non-alive response here, with no retry.
                return RequestKernelId(transport, clock, cancellation, request_id);
            })
        .and_then(
            [&](const std::optional<bytes::Bytes>& poll) -> Status
            {
                if (!poll.has_value())
                {
                    return Fail(ErrorKind::kTimeout, "kernel did not respond after upload");
                }
                if (!LooksKernelAlive(*poll))
                {
                    return Fail(ErrorKind::kBadResponse, "kernel did not report a valid ID after upload");
                }
                events.Log(LogLevel::kInfo, "Kernel is alive");
                return {};
            });
}

// read_mem(), ("BEEF/READ_EEPROM" combined-request protocol).
Result<bytes::Bytes> DensoSh705xEepromCanExecutor::ReadMem(ICanFlashTransport& transport, IClock& clock,
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
    events.Progress(0, static_cast<int>(length));

    while (willget != 0)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during EEPROM read");
        }

        constexpr std::uint32_t kNumBlocks = 1; // legacy hardcodes this per outer iteration
        Result<bytes::Bytes> page = ReadEepromPage(transport, cancellation, request_id, mode, addr, pagesize);
        if (!page.has_value())
        {
            return std::unexpected(page.error());
        }
        // Unlike the K-Line sibling, mapdata is never sliced by cplen/
        // skip_start here -- it just accumulates whatever survived each
        // response's own framing strip, whole (Task 7 report).
        mapdata.append_range(*page);

        std::uint32_t cplen = kNumBlocks * pagesize - skip_start;
        skip_start = 0;

        if (Status slept = clock.Sleep(std::chrono::milliseconds{kInterPageDelayMs}, cancellation); !slept.has_value())
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

        events.Progress(static_cast<int>(len_done), static_cast<int>(length));
    }

    events.Progress(static_cast<int>(length), static_cast<int>(length));
    return mapdata;
}

} // namespace fastecu::flash
