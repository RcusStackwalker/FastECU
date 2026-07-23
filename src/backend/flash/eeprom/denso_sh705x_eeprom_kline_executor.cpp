#include "src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.h"

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace fastecu::flash
{
namespace
{

// ---------------------------------------------------------------------
// Literal protocol constants transcribed from
// src/backend/definitions/kernelcomms.h.
// ---------------------------------------------------------------------
constexpr std::uint16_t kSubKernelStartComm = 0xbeef; // SUB_KERNEL_START_COMM
constexpr std::uint8_t kSubKernelId = 0x01;           // SUB_KERNEL_ID
constexpr std::uint8_t kSidDump = 0xbd;               // SID_DUMP

constexpr std::uint32_t kUploadChunkBytes = 0x80; // send_sid_36_transferdata's blocksize
constexpr std::uint32_t kEepromBlockBytes = 32;   // NP10 EEPROM block size
constexpr std::uint32_t kNp10MaxBlocks = 32;      // NP10_MAXBLKS

// The only two of the legacy class's seven declared timeout members that
// any call site actually uses (verified by grep over the deleted .cpp);
// receive_timeout/serial_read_extra_short_timeout/serial_read_short_timeout/
// serial_read_medium_timeout/serial_read_long_timeout are dead members, not
// carried forward.
constexpr int kExtraLongTimeoutMs = 3000; // serial_read_extra_long_timeout
constexpr int kShortTimeoutMs = 2000;     // serial_read_timeout

// delay()/QThread::msleep() call sites, transcribed 1:1 from
// eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp's line numbers noted
// at each use below.
constexpr int kProbeSettleDelayMs = 100;       // connect_bootloader():169
constexpr int kInitSettleDelayMs = 100;        // connect_bootloader():200
constexpr int kKernelIdRequestDelayMs = 200;   // request_kernel_id():986
constexpr int kKernelAliveSettleDelayMs = 100; // upload_kernel():438
constexpr int kKernelPollRetryDelayMs = 200;   // upload_kernel():443
constexpr int kMaxKernelAlivePollIterations = 10;

constexpr int kReadMemPreReadDelayMs = 500;  // read_mem():523
constexpr int kReadMemPollDelayMs = 100;     // read_mem():532
constexpr int kReadMemInterBlockDelayMs = 1; // read_mem():582
constexpr int kMaxReadMemInnerRetries = 5;   // read_mem():526 (`timeout < 5`)

// ---------------------------------------------------------------------
// Request builders -- transcribed from each send_sid_*()/request_kernel_id()
// body (see task-6-report.md's table for the exact legacy line ranges).
// ---------------------------------------------------------------------

bytes::Bytes frame(bytes::ByteView payload, std::uint8_t tester_id, std::uint8_t target_id)
{
    return SsmProtocol::addHeader(payload, tester_id, target_id, false);
}

bytes::Bytes sid_bf_request()
{
    return {0xbf};
}
bytes::Bytes sid_81_request()
{
    return {0x81};
}
bytes::Bytes sid_83_request()
{
    return {0x83, 0x00};
}
bytes::Bytes sid_27_request_seed_request()
{
    return {0x27, 0x01};
}
bytes::Bytes sid_27_send_key_request(bytes::ByteView key)
{
    bytes::Bytes out{0x27, 0x02};
    out.insert(out.end(), key.begin(), key.end());
    return out;
}
bytes::Bytes sid_10_request()
{
    return {0x10, 0x85, 0x02};
}
bytes::Bytes sid_34_request(std::uint32_t addr, std::uint32_t len)
{
    return {
        0x34,
        static_cast<bytes::Byte>((addr >> 16) & 0xFF),
        static_cast<bytes::Byte>((addr >> 8) & 0xFF),
        static_cast<bytes::Byte>(addr & 0xFF),
        0x04,
        static_cast<bytes::Byte>((len >> 16) & 0xFF),
        static_cast<bytes::Byte>((len >> 8) & 0xFF),
        static_cast<bytes::Byte>(len & 0xFF),
    };
}
bytes::Bytes sid_31_request()
{
    return {0x31, 0x01, 0x01};
}

// request_kernel_id(), lines 964-994: NOT SsmProtocol::addHeader-framed.
bytes::Bytes request_kernel_id_frame()
{
    bytes::Bytes out{
        static_cast<bytes::Byte>((kSubKernelStartComm >> 8) & 0xFF),
        static_cast<bytes::Byte>(kSubKernelStartComm & 0xFF),
        static_cast<bytes::Byte>((std::uint16_t(1) >> 8) & 0xFF), // (datalen(0)+1)>>8
        static_cast<bytes::Byte>(1 & 0xFF),                       // (datalen(0)+1)&0xFF
        kSubKernelId,
    };
    out.push_back(SsmProtocol::checksum(out, false));
    return out;
}

bool looks_kernel_alive(bytes::ByteView received)
{
    return received.size() > 4 && received[0] == static_cast<bytes::Byte>((kSubKernelStartComm >> 8) & 0xFF) &&
           received[1] == static_cast<bytes::Byte>(kSubKernelStartComm & 0xFF) &&
           received[4] == static_cast<bytes::Byte>(kSubKernelId | 0x40);
}

// generate_seed_key(), lines 854-879 (the non-"_ecutek" / stock branch).
bytes::Bytes generate_stock_seed_key(bytes::ByteView seed)
{
    static constexpr std::uint16_t kIndex[] = {
        0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608,
        0xAFB8, 0x503D, 0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0};
    static constexpr std::uint8_t kTransform[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}

// generate_ecutek_seed_key(), lines 886-911: same key table as stock, a
// different indextransformation table (first byte 0x4 vs 0x5, etc).
bytes::Bytes generate_ecutek_seed_key(bytes::ByteView seed)
{
    static constexpr std::uint16_t kIndex[] = {
        0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608,
        0xAFB8, 0x503D, 0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0};
    static constexpr std::uint8_t kTransform[] = {
        0x4, 0x2, 0x5, 0x1, 0x8, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}

// encrypt_payload(), lines 923-939.
bytes::Bytes encrypt_kernel_payload(bytes::ByteView buf, std::uint32_t len)
{
    static constexpr std::uint16_t kIndex[] = {0x7856, 0xCE22, 0xF513, 0x6E86};
    static constexpr std::uint8_t kTransform[] = {
        0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
        0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    return SsmProtocol::calculatePayload(buf, len, kIndex, kTransform);
}

// ---------------------------------------------------------------------
// Shared write/read helpers.
// ---------------------------------------------------------------------

// Shared shape of every SSM-addHeader-framed exchange (send_sid_bf/81/83/
// 27req/27key/10/34/31 and each send_sid_36_transferdata block): write,
// then read with no delay in between (legacy never delays here), mapping a
// timed-out response (no frame at all) to ErrorKind::Timeout -- a genuine
// hard transport failure (Disconnected et al.) propagates as-is, and
// cancellation is checked before the write and after the read, per the
// portable seam's cancellation contract.
Result<bytes::Bytes> ssm_exchange(IKlineFlashTransport& transport, IClock&,
                                  const ICancellationToken& cancellation, bytes::ByteView payload,
                                  std::uint8_t tester_id, std::uint8_t target_id, int timeout_ms)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before write");
    }
    const bytes::Bytes framed = frame(payload, tester_id, target_id);
    Result<std::size_t> written = transport.write(framed);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after write");
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
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU");
    }
    return std::move(**received);
}

// request_kernel_id(), lines 964-994 (write) + 984-987 (delay(200) then
// read). Unlike ssm_exchange(), an empty response here is NOT an error --
// it is the normal "kernel not (yet) running" signal that both
// connect_bootloader()'s initial probe and upload_kernel()'s alive-poll loop
// interpret themselves.
Result<bytes::Bytes> request_kernel_id(IKlineFlashTransport& transport, IClock& clock,
                                       const ICancellationToken& cancellation)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before write");
    }
    Result<std::size_t> written = transport.write(request_kernel_id_frame());
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (Status slept = clock.sleep(kKernelIdRequestDelayMs, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after delay");
    }
    auto received = transport.read(kShortTimeoutMs, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after read");
    }
    if (!received->has_value())
    {
        return bytes::Bytes{}; // no frame -- "not (yet) alive", not an error here
    }
    return std::move(**received);
}

// send_sid_36_transferdata(), lines 755-831: transfers `encrypted` in
// kUploadChunkBytes (128-byte) blocks, cancellation-checked "between
// chunks" per the Global Constraints. `len` is mutated across iterations
// exactly as legacy does (decremented on every non-final block so the final
// block's byte count is whatever remains) -- this looks unusual but is a
// faithful transcription, not a bug: maxblocks is computed once up front
// from the original len, so the loop bound itself never changes.
Status transfer_data_blocks(IKlineFlashTransport& transport, IClock& clock,
                            const ICancellationToken& cancellation, std::uint8_t tester_id,
                            std::uint8_t target_id, std::uint32_t addr, bytes::ByteView encrypted,
                            std::uint32_t len)
{
    len &= ~std::uint32_t(3);
    if (encrypted.empty() || len == 0)
    {
        return fail(ErrorKind::Internal, "kernel transfer payload is empty");
    }

    const std::uint32_t maxblocks = (len - 1) / kUploadChunkBytes;
    for (std::uint32_t blockno = 0; blockno <= maxblocks; ++blockno)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled between kernel transfer chunks");
        }

        const std::uint32_t block_addr = addr + blockno * kUploadChunkBytes;
        bytes::Bytes payload{
            0x36,
            static_cast<bytes::Byte>((block_addr >> 16) & 0xFF),
            static_cast<bytes::Byte>((block_addr >> 8) & 0xFF),
            static_cast<bytes::Byte>(block_addr & 0xFF),
        };
        if (blockno == maxblocks)
        {
            for (std::uint32_t i = 0; i < len; ++i)
            {
                payload.push_back(encrypted[i + blockno * kUploadChunkBytes]);
            }
        }
        else
        {
            for (std::uint32_t i = 0; i < kUploadChunkBytes; ++i)
            {
                payload.push_back(encrypted[i + blockno * kUploadChunkBytes]);
            }
            len -= kUploadChunkBytes;
        }

        Result<bytes::Bytes> resp = ssm_exchange(transport, clock, cancellation, payload, tester_id,
                                                 target_id, kShortTimeoutMs);
        if (!resp.has_value())
        {
            return std::unexpected(resp.error());
        }
        if (resp->size() < 5 || (*resp)[4] != 0x76)
        {
            return fail(ErrorKind::BadResponse, "kernel transfer block rejected");
        }
    }
    return {};
}

} // namespace

Result<FlashExecutionResult> DensoSh705xEepromKlineExecutor::execute(
    const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status match = check_family_transport_match(plan, FlashFamily::DensoSh705xEepromKline,
                                                    TransportKind::Kline);
        !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }

    // Checked downcast, not static_cast: the plan's declared transport kind
    // and the concrete object the caller actually passed are two different
    // facts. A mismatch here must fail with InvalidConfig, not corrupt
    // memory via an unchecked static_cast to the wrong dynamic type.
    auto *kline_transport_ptr = dynamic_cast<IKlineFlashTransport *>(&transport);
    if (kline_transport_ptr == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "transport does not implement IKlineFlashTransport");
    }
    IKlineFlashTransport& kline_transport = *kline_transport_ptr;
    const auto& kline_plan = std::get<DensoSh705xEepromKlinePlan>(plan.family_plan());

    if (Status configured = kline_transport.configure(KlineConfig{
            .baud = kline_plan.initial_baud,
            .iso14230 = false,
            .tester_id = kline_plan.tester_id,
            .target_id = kline_plan.target_id,
        });
        !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    if (Status opened = kline_transport.open(); !opened.has_value())
    {
        return std::unexpected(opened.error());
    }

    // Ensures exactly-once close on every exit path below.
    struct ScopedClose
    {
        IKlineFlashTransport& t;
        bool done = false;
        ~ScopedClose()
        {
            if (!done)
            {
                t.close();
            }
        }
    } scoped_close{kline_transport};

    bool kernel_alive = false;
    if (Status connected = connect_bootloader(kline_transport, clock, cancellation, events,
                                              kline_plan, kernel_alive);
        !connected.has_value())
    {
        Status close_status = kline_transport.close();
        scoped_close.done = true;
        if (!close_status.has_value())
        {
            events.log(LogLevel::Warning, "close failed after connect_bootloader error");
        }
        return std::unexpected(connected.error());
    }

    if (!kernel_alive)
    {
        if (Status uploaded = upload_kernel(kline_transport, clock, cancellation, events, kline_plan,
                                            plan.kernel());
            !uploaded.has_value())
        {
            Status close_status = kline_transport.close();
            scoped_close.done = true;
            if (!close_status.has_value())
            {
                events.log(LogLevel::Warning, "close failed after upload_kernel error");
            }
            return std::unexpected(uploaded.error());
        }
    }

    Result<bytes::Bytes> read_result =
        read_mem(kline_transport, clock, cancellation, events, plan.transfer_region(), kline_plan.mode);

    Status close_status = kline_transport.close();
    scoped_close.done = true;

    if (!read_result.has_value())
    {
        return std::unexpected(read_result.error());
    }
    if (!close_status.has_value())
    {
        // Main error wins over close error; close-only error is returned.
        return std::unexpected(close_status.error());
    }

    return FlashExecutionResult{
        .operation = plan.operation(),
        .read_bytes = std::move(*read_result),
    };
}

// connect_bootloader(), lines 152-296.
Status DensoSh705xEepromKlineExecutor::connect_bootloader(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, const DensoSh705xEepromKlinePlan& kline_plan, bool& kernel_alive)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before connect");
    }

    // line 167: probe at bootloader speed first.
    if (Status baud = transport.setBaud(62500); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    if (Status slept = clock.sleep(kProbeSettleDelayMs, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after probe delay");
    }

    events.log(LogLevel::Info, "Checking if kernel is already running...");
    Result<bytes::Bytes> probe = request_kernel_id(transport, clock, cancellation);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (looks_kernel_alive(*probe))
    {
        // lines 185-189: any well-formed alive response short-circuits the
        // rest of connect_bootloader() -- upload_kernel() is skipped too.
        kernel_alive = true;
        events.log(LogLevel::Info, "Kernel already running");
        return {};
    }
    // lines 179-194: BOTH "no frame at all" and "frame present but markers
    // wrong" fall through to the full init sequence below -- neither is a
    // hard failure at this point.
    events.log(LogLevel::Warning, "No response from kernel, initialising ECU...");

    // lines 198-200: drop back to 4800 baud for the SSM init handshake.
    if (Status baud = transport.setBaud(4800); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    if (Status slept = clock.sleep(kInitSettleDelayMs, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after init delay");
    }

    const std::uint8_t tester_id = kline_plan.tester_id;
    const std::uint8_t target_id = kline_plan.target_id;

    events.log(LogLevel::Info, "Initializing K-Line communications");
    Result<bytes::Bytes> bf = ssm_exchange(transport, clock, cancellation, sid_bf_request(), tester_id,
                                           target_id, kExtraLongTimeoutMs);
    if (!bf.has_value())
    {
        return std::unexpected(bf.error());
    }
    if (bf->size() < 5 || (*bf)[4] != 0xFF)
    {
        return fail(ErrorKind::BadResponse, "SID BF (SSM init) rejected");
    }

    events.log(LogLevel::Info, "Requesting to start communication");
    Result<bytes::Bytes> start_comm = ssm_exchange(transport, clock, cancellation, sid_81_request(),
                                                   tester_id, target_id, kExtraLongTimeoutMs);
    if (!start_comm.has_value())
    {
        return std::unexpected(start_comm.error());
    }
    if (start_comm->size() < 5 || (*start_comm)[4] != 0xC1)
    {
        return fail(ErrorKind::BadResponse, "SID 81 (start communication) rejected");
    }

    events.log(LogLevel::Info, "Requesting timings params");
    Result<bytes::Bytes> timings = ssm_exchange(transport, clock, cancellation, sid_83_request(),
                                                tester_id, target_id, kExtraLongTimeoutMs);
    if (!timings.has_value())
    {
        return std::unexpected(timings.error());
    }
    if (timings->size() < 5 || (*timings)[4] != 0xC3)
    {
        return fail(ErrorKind::BadResponse, "SID 83 (request timings) rejected");
    }

    events.log(LogLevel::Info, "Requesting seed");
    Result<bytes::Bytes> seed_resp = ssm_exchange(transport, clock, cancellation,
                                                  sid_27_request_seed_request(), tester_id, target_id,
                                                  kExtraLongTimeoutMs);
    if (!seed_resp.has_value())
    {
        return std::unexpected(seed_resp.error());
    }
    if (seed_resp->size() < 10 || (*seed_resp)[4] != 0x67)
    {
        return fail(ErrorKind::BadResponse, "SID 27 (request seed) rejected");
    }
    const bytes::Bytes seed(seed_resp->begin() + 6, seed_resp->begin() + 10);

    const bytes::Bytes seed_key = kline_plan.security == DensoSecurityVariant::EcuTek
                                      ? generate_ecutek_seed_key(seed)
                                      : generate_stock_seed_key(seed);

    events.log(LogLevel::Info, "Sending seed key to ECU");
    Result<bytes::Bytes> key_resp = ssm_exchange(transport, clock, cancellation,
                                                 sid_27_send_key_request(seed_key), tester_id, target_id,
                                                 kExtraLongTimeoutMs);
    if (!key_resp.has_value())
    {
        return std::unexpected(key_resp.error());
    }
    // line 277: same positive-response code (0x67) as the request-seed step.
    if (key_resp->size() < 5 || (*key_resp)[4] != 0x67)
    {
        return fail(ErrorKind::BadResponse, "SID 27 (send seed key) rejected");
    }

    events.log(LogLevel::Info, "Set session mode");
    Result<bytes::Bytes> diag = ssm_exchange(transport, clock, cancellation, sid_10_request(), tester_id,
                                             target_id, kExtraLongTimeoutMs);
    if (!diag.has_value())
    {
        return std::unexpected(diag.error());
    }
    if (diag->size() < 5 || (*diag)[4] != 0x50)
    {
        return fail(ErrorKind::BadResponse, "SID 10 (start diagnostic) rejected");
    }

    events.log(LogLevel::Info, "Successfully set to programming session");
    return {};
}

// upload_kernel(), lines 303-446.
Status DensoSh705xEepromKlineExecutor::upload_kernel(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, const DensoSh705xEepromKlinePlan& kline_plan, const KernelImage& kernel)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before kernel upload");
    }
    if (kernel.bytes.empty())
    {
        return fail(ErrorKind::InvalidConfig, "kernel image is empty");
    }

    const std::uint32_t start_address = kernel.load_address;
    const std::uint32_t pl_len =
        (static_cast<std::uint32_t>(kernel.bytes.size()) + 3) & ~std::uint32_t(3);

    // INTENTIONAL DEVIATION from legacy (documented per Global Constraints,
    // not a silent behavior change): eeprom_ecu_subaru_denso_sh705x_kline_
    // operation.cpp:345-347,375 rounds pl_len UP but then calls
    // encrypt_payload() on the ORIGINAL, unpadded file buffer.
    // SsmProtocol::calculatePayload() silently clamps its output to
    // buf.size()&~3 whenever the requested len exceeds buf.size()
    // (ssm_protocol_core.cpp:96-100), and send_sid_36_transferdata() then
    // indexes the encrypted buffer up to pl_len-1 regardless -- an
    // out-of-bounds read on any kernel file whose length isn't already a
    // multiple of 4 (see task-6-report.md, "Legacy behavior surprises" #1).
    // Fix: pad the *source* buffer up to pl_len with zero bytes before
    // encrypting, so buf.size() == len == pl_len and calculatePayload()
    // never truncates. For every kernel image this task's tests use (already
    // 4-byte aligned), padded_kernel == kernel.bytes and this is a no-op --
    // byte-for-byte identical to the legacy trace.
    bytes::Bytes padded_kernel = kernel.bytes;
    padded_kernel.resize(pl_len, 0);

    const std::uint8_t tester_id = kline_plan.tester_id;
    const std::uint8_t target_id = kline_plan.target_id;

    // line 361: change port speed to upload kernel.
    if (Status baud = transport.setBaud(kline_plan.kernel_baud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }

    events.log(LogLevel::Info, "Requesting kernel upload");
    Result<bytes::Bytes> upload_req = ssm_exchange(transport, clock, cancellation,
                                                   sid_34_request(start_address, pl_len), tester_id,
                                                   target_id, kExtraLongTimeoutMs);
    if (!upload_req.has_value())
    {
        return std::unexpected(upload_req.error());
    }
    if (upload_req->size() < 5 || (*upload_req)[4] != 0x74)
    {
        return fail(ErrorKind::BadResponse, "kernel upload request rejected");
    }

    const bytes::Bytes encrypted_kernel = encrypt_kernel_payload(padded_kernel, pl_len);
    events.log(LogLevel::Info, "Transfer kernel data");
    if (Status transferred = transfer_data_blocks(transport, clock, cancellation, tester_id, target_id,
                                                  start_address, encrypted_kernel, pl_len);
        !transferred.has_value())
    {
        return std::unexpected(transferred.error());
    }

    // lines 386-405: checksum-bypass 4-byte trailer.
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before checksum bypass");
    }
    Result<bytes::Bytes> bypass_req = ssm_exchange(transport, clock, cancellation,
                                                   sid_34_request(start_address + pl_len, 4), tester_id,
                                                   target_id, kExtraLongTimeoutMs);
    if (!bypass_req.has_value())
    {
        return std::unexpected(bypass_req.error());
    }
    if (bypass_req->size() < 5 || (*bypass_req)[4] != 0x74)
    {
        return fail(ErrorKind::BadResponse, "checksum bypass request rejected");
    }

    const bytes::Bytes cks_bypass{0x00, 0x00, 0x5A, 0xA5};
    const bytes::Bytes encrypted_bypass = encrypt_kernel_payload(cks_bypass, 4);
    if (Status transferred = transfer_data_blocks(transport, clock, cancellation, tester_id, target_id,
                                                  start_address + pl_len, encrypted_bypass, 4);
        !transferred.has_value())
    {
        return std::unexpected(transferred.error());
    }

    events.log(LogLevel::Info, "Kernel uploaded");

    // line 410-415: jump to kernel. Cancellation is checked immediately
    // before this write per the portable seam's cancellation contract
    // ("before kernel jump").
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before kernel jump");
    }
    events.log(LogLevel::Info, "Jump to kernel");
    Result<bytes::Bytes> jump = ssm_exchange(transport, clock, cancellation, sid_31_request(), tester_id,
                                             target_id, kExtraLongTimeoutMs);
    if (!jump.has_value())
    {
        return std::unexpected(jump.error());
    }
    if (jump->size() < 5 || (*jump)[4] != 0x71)
    {
        return fail(ErrorKind::BadResponse, "kernel start routine rejected");
    }

    events.log(LogLevel::Info, "Kernel started, initializing...");
    if (Status baud = transport.setBaud(62500); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }

    // lines 422-445: up to 10 iterations, 200ms between attempts, 100ms
    // extra settle once alive.
    events.log(LogLevel::Info, "Requesting kernel ID...");
    for (int i = 0; i < kMaxKernelAlivePollIterations; ++i)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during kernel-alive poll");
        }
        Result<bytes::Bytes> poll = request_kernel_id(transport, clock, cancellation);
        if (!poll.has_value())
        {
            return std::unexpected(poll.error());
        }
        if (looks_kernel_alive(*poll))
        {
            if (Status slept = clock.sleep(kKernelAliveSettleDelayMs, cancellation); !slept.has_value())
            {
                return std::unexpected(slept.error());
            }
            events.log(LogLevel::Info, "Kernel is alive");
            return {};
        }
        if (Status slept = clock.sleep(kKernelPollRetryDelayMs, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    return fail(ErrorKind::Timeout, "kernel did not respond after upload");
}

// read_mem(), lines 453-602 ("nisprog kernel" SID_DUMP protocol).
Result<bytes::Bytes> DensoSh705xEepromKlineExecutor::read_mem(
    IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
    IEventSink& events, const MemoryRegion& region, EepromReadMode mode)
{
    const std::uint32_t start_addr = region.start;
    const std::uint32_t length = region.length;

    std::uint32_t skip_start = start_addr & (kEepromBlockBytes - 1);
    std::uint32_t addr = start_addr - skip_start;
    std::uint32_t willget = (skip_start + length + kEepromBlockBytes - 1) & ~(kEepromBlockBytes - 1);
    std::uint32_t len_done = 0;

    bytes::Bytes mapdata;
    events.progress(0, static_cast<int>(length));

    while (willget != 0)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during EEPROM read");
        }

        std::uint32_t numblocks = willget / kEepromBlockBytes;
        if (numblocks > kNp10MaxBlocks)
        {
            numblocks = kNp10MaxBlocks;
        }
        const std::uint32_t curblock = addr / kEepromBlockBytes;
        const std::uint32_t pagesize = numblocks * kEepromBlockBytes + numblocks * 3;

        const bytes::Bytes request{
            kSidDump,
            static_cast<bytes::Byte>(mode),
            static_cast<bytes::Byte>((numblocks >> 8) & 0xFF),
            static_cast<bytes::Byte>(numblocks & 0xFF),
            static_cast<bytes::Byte>((curblock >> 8) & 0xFF),
            static_cast<bytes::Byte>(curblock & 0xFF),
        };

        Result<std::size_t> written = transport.write(request);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        if (Status slept = clock.sleep(kReadMemPreReadDelayMs, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled after EEPROM request delay");
        }

        bytes::Bytes pagedata;
        int timeout = 0;
        while (pagedata.size() < pagesize && timeout < kMaxReadMemInnerRetries)
        {
            if (cancellation.cancelled())
            {
                return fail(ErrorKind::Cancelled, "cancelled during EEPROM page read");
            }
            if (Status slept = clock.sleep(kReadMemPollDelayMs, cancellation); !slept.has_value())
            {
                return std::unexpected(slept.error());
            }
            auto received = transport.read(kShortTimeoutMs, cancellation);
            if (!received.has_value())
            {
                return std::unexpected(received.error());
            }
            if (cancellation.cancelled())
            {
                return fail(ErrorKind::Cancelled, "cancelled after EEPROM page read");
            }
            if (received->has_value())
            {
                pagedata.insert(pagedata.end(), (*received)->begin(), (*received)->end());
            }
            else
            {
                ++timeout;
            }
        }
        // line 551: `if (timeout >= 1000)` is legacy dead code -- the loop
        // above can only ever reach timeout==kMaxReadMemInnerRetries (5), so
        // it never trips. A page that never fully arrives is therefore
        // silently accepted short rather than erroring; preserved
        // faithfully here (this is not the documented OOB-read fix this
        // task authorizes -- that fix is scoped to kernel upload only).

        // lines 545-550: strip the [2-byte prefix][32 data][1 trailing]
        // framing per block. Bounds-checked rather than mirroring Qt's
        // clamping remove() calls, since pagedata can be short when the
        // dead-code path above lets a partial page through -- both
        // approaches take "however much arrived" and neither indexes out of
        // range.
        for (std::uint32_t block = 0; block < numblocks; ++block)
        {
            const std::size_t raw_offset = static_cast<std::size_t>(block) * 35;
            if (raw_offset + 2 >= pagedata.size())
            {
                break;
            }
            const std::size_t data_begin = raw_offset + 2;
            const std::size_t data_end = std::min(data_begin + kEepromBlockBytes, pagedata.size());
            mapdata.insert(mapdata.end(), pagedata.begin() + static_cast<std::ptrdiff_t>(data_begin),
                           pagedata.begin() + static_cast<std::ptrdiff_t>(data_end));
        }

        std::uint32_t cplen = numblocks * kEepromBlockBytes - skip_start;
        skip_start = 0;

        if (Status slept = clock.sleep(kReadMemInterBlockDelayMs, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }

        const std::uint32_t extrabytes = cplen + len_done;
        if (extrabytes > length)
        {
            cplen -= (extrabytes - length);
        }
        len_done += cplen;
        addr += numblocks * kEepromBlockBytes;
        willget -= numblocks * kEepromBlockBytes;

        events.progress(static_cast<int>(len_done), static_cast<int>(length));
    }

    events.progress(static_cast<int>(length), static_cast<int>(length));
    return mapdata;
}

} // namespace fastecu::flash
