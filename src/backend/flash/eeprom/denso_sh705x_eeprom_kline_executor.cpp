#include "src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_types.h"

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/denso_sh705x_kline_common.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/ports/logged_step.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <array>

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::ComposeBeWithChecksum;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;
using OptionalBytes = IKlineFlashTransport::OptionalBytes;

// ---------------------------------------------------------------------
// Literal protocol constants transcribed from
// src/backend/flash/kernel/kernelcomms.h.
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
constexpr std::chrono::milliseconds kExtraLongTimeout = 3000ms; // serial_read_extra_long_timeout
constexpr std::chrono::milliseconds kShortTimeout = 2000ms;     // serial_read_timeout

// Legacy delay()/QThread::msleep() durations, transcribed 1:1.
constexpr std::chrono::milliseconds kProbeSettleDelay = 100ms;
constexpr std::chrono::milliseconds kInitSettleDelay = 100ms;
constexpr std::chrono::milliseconds kKernelIdRequestDelay = 200ms;
constexpr std::chrono::milliseconds kKernelAliveSettleDelay = 100ms;
constexpr std::chrono::milliseconds kKernelPollRetryDelay = 200ms;
constexpr int kMaxKernelAlivePollIterations = 10;

constexpr std::chrono::milliseconds kReadMemPreReadDelay = 500ms;
constexpr std::chrono::milliseconds kReadMemPollDelay = 100ms;
constexpr std::chrono::milliseconds kReadMemInterBlockDelay = 1ms;
constexpr int kMaxReadMemInnerRetries = 5; // `timeout < 5`

// ---------------------------------------------------------------------
// Request builders -- transcribed from each send_sid_*()/request_kernel_id()
// body (see task-6-report.md's table for the exact legacy line ranges).
// ---------------------------------------------------------------------

bytes::Bytes Frame(bytes::ByteView payload, std::uint8_t tester_id, std::uint8_t target_id)
{
    return ssm_protocol::AddHeader(payload, tester_id, target_id);
}

bytes::Bytes SidBfRequest()
{
    return {0xbf};
}
bytes::Bytes Sid81Request()
{
    return {0x81};
}
bytes::Bytes Sid83Request()
{
    return {0x83, 0x00};
}
bytes::Bytes Sid27RequestSeedRequest()
{
    return {0x27, 0x01};
}
bytes::Bytes Sid27SendKeyRequest(bytes::ByteView key)
{
    return ComposeBe(0x27_b, 0x02_b, key);
}
bytes::Bytes Sid10Request()
{
    return {0x10, 0x85, 0x02};
}
std::array<bytes::Byte, 8> Sid34Request(std::uint32_t addr, std::uint32_t len)
{
    return ComposeBe(0x34_b, U24{addr}, 0x04_b, U24{len});
}
bytes::Bytes Sid31Request()
{
    return {0x31, 0x01, 0x01};
}

// request_kernel_id(): NOT SsmProtocol::addHeader-framed.
std::array<bytes::Byte, 6> RequestKernelIdFrame()
{
    return ComposeBeWithChecksum(bytes::Sum8, kSubKernelStartComm, std::uint16_t{1}, kSubKernelId);
}

bool LooksKernelAlive(bytes::ByteView received)
{
    return received.size() > 4 && bytes::ReadU16Be(received, 0) == kSubKernelStartComm &&
           received[4] == static_cast<bytes::Byte>(kSubKernelId | 0x40U);
}

// ---------------------------------------------------------------------
// Shared write/read helpers.
// ---------------------------------------------------------------------

// Shared shape of every SSM-addHeader-framed exchange (send_sid_bf/81/83/
// 27req/27key/10/34/31 and each send_sid_36_transferdata block): write,
// then read with no delay in between (legacy never delays here), mapping a
// timed-out response (no frame at all) to ErrorKind::kTimeout -- a genuine
// hard transport failure (Disconnected et al.) propagates as-is, and
// cancellation is checked before the write and after the read, per the
// portable seam's cancellation contract.
Result<bytes::Bytes> SsmExchange(IKlineFlashTransport& transport, IClock&, const ICancellationToken& cancellation,
                                 bytes::ByteView payload, std::uint8_t tester_id, std::uint8_t target_id,
                                 std::chrono::milliseconds timeout)
{
    return CheckCancelled(cancellation, "cancelled before write")
        .and_then([&] { return transport.Write(Frame(payload, tester_id, target_id)); })
        .and_then([&](std::size_t) { return CheckCancelled(cancellation, "cancelled after write"); })
        .and_then([&] { return transport.Read(timeout, cancellation); })
        .and_then(
            [&](OptionalBytes received)
            {
                return CheckCancelled(cancellation, "cancelled after read")
                    .transform([&] { return std::move(received); });
            })
        .and_then(
            [](OptionalBytes received) -> Result<bytes::Bytes>
            {
                if (!received.has_value())
                {
                    return Fail(ErrorKind::kTimeout, "no response from ECU");
                }
                return std::move(*received);
            });
}

// request_kernel_id(): write, delay(200), then read. Unlike ssm_exchange(), an empty response here is NOT an error --
// it is the normal "kernel not (yet) running" signal that both
// connect_bootloader()'s initial probe and upload_kernel()'s alive-poll loop
// interpret themselves.
Result<bytes::Bytes> RequestKernelId(IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation)
{
    return CheckCancelled(cancellation, "cancelled before write")
        .and_then([&] { return transport.Write(RequestKernelIdFrame()); })
        .and_then([&](std::size_t) { return clock.Sleep(kKernelIdRequestDelay, cancellation); })
        .and_then([&] { return CheckCancelled(cancellation, "cancelled after delay"); })
        .and_then([&] { return transport.Read(kShortTimeout, cancellation); })
        .and_then(
            [&](OptionalBytes received)
            {
                return CheckCancelled(cancellation, "cancelled after read")
                    .transform([&] { return std::move(received); });
            })
        // No frame -- "not (yet) alive", not an error here.
        .transform([](OptionalBytes received) { return std::move(received).value_or(bytes::Bytes{}); });
}

// send_sid_36_transferdata(): transfers `encrypted` in
// kUploadChunkBytes (128-byte) blocks, cancellation-checked "between
// chunks" per the Global Constraints. `len` is mutated across iterations
// exactly as legacy does (decremented on every non-final block so the final
// block's byte count is whatever remains) -- this looks unusual but is a
// faithful transcription, not a bug: maxblocks is computed once up front
// from the original len, so the loop bound itself never changes.
Status TransferDataBlocks(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                          std::uint8_t tester_id, std::uint8_t target_id, std::uint32_t addr, bytes::ByteView encrypted,
                          std::uint32_t len)
{
    len &= ~std::uint32_t(3);
    if (encrypted.empty() || len == 0)
    {
        return Fail(ErrorKind::kInternal, "kernel transfer payload is empty");
    }

    const std::uint32_t maxblocks = (len - 1) / kUploadChunkBytes;
    for (std::uint32_t blockno = 0; blockno <= maxblocks; ++blockno)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled between kernel transfer chunks");
        }

        const std::uint32_t block_addr = addr + blockno * kUploadChunkBytes;
        bytes::Bytes payload = bytes::ComposeBeWithExtraCapacity(kUploadChunkBytes, 0x36_b, U24{block_addr});
        const std::size_t chunk_offset = static_cast<std::size_t>(blockno) * kUploadChunkBytes;
        if (blockno == maxblocks)
        {
            std::ranges::copy(encrypted.subspan(chunk_offset, len), std::back_inserter(payload));
        }
        else
        {
            std::ranges::copy(encrypted.subspan(chunk_offset, kUploadChunkBytes), std::back_inserter(payload));
            len -= kUploadChunkBytes;
        }

        Result<bytes::Bytes> resp =
            SsmExchange(transport, clock, cancellation, payload, tester_id, target_id, kShortTimeout);
        if (!resp.has_value())
        {
            return std::unexpected(resp.error());
        }
        if (resp->size() < 5 || (*resp)[4] != 0x76)
        {
            return Fail(ErrorKind::kBadResponse, "kernel transfer block rejected");
        }
    }
    return {};
}

// An SSM exchange at the extra-long timeout whose reply must be at least
// `min_size` bytes and carry `code` at byte 4.
Result<bytes::Bytes> Require(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                             const DensoSh705xEepromKlinePlan& kline_plan, bytes::ByteView payload, bytes::Byte code,
                             std::string_view rejected, std::size_t min_size = 5)
{
    Result<bytes::Bytes> response = SsmExchange(transport, clock, cancellation, payload, kline_plan.tester_id,
                                                kline_plan.target_id, kExtraLongTimeout);
    if (!response.has_value())
    {
        return response;
    }
    if (response->size() < min_size || (*response)[4] != code)
    {
        return Fail(ErrorKind::kBadResponse, std::string(rejected));
    }
    return response;
}

Status SetBaudAndSettle(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                        int baud, std::chrono::milliseconds settle, std::string_view cancelled_after)
{
    return transport.SetBaud(baud)
        .and_then([&] { return clock.Sleep(settle, cancellation); })
        .and_then([&] { return CheckCancelled(cancellation, std::string(cancelled_after)); });
}

// The SSM init handshake, seed/key and programming session.
Status InitProgrammingSession(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const DensoSh705xEepromKlinePlan& kline_plan)
{
    struct Step
    {
        std::string_view label;
        bytes::Bytes request;
        bytes::Byte code;
        std::string_view rejected;
    };
    for (const Step& step :
         {Step{"Initializing K-Line communications", SidBfRequest(), 0xFF, "SID BF (SSM init) rejected"},
          Step{"Requesting to start communication", Sid81Request(), 0xC1, "SID 81 (start communication) rejected"},
          Step{"Requesting timings params", Sid83Request(), 0xC3, "SID 83 (request timings) rejected"}})
    {
        events.Log(LogLevel::kInfo, step.label);
        if (auto response = Require(transport, clock, cancellation, kline_plan, step.request, step.code, step.rejected);
            !response.has_value())
        {
            return std::unexpected(response.error());
        }
    }
    events.Log(LogLevel::kInfo, "Requesting seed");
    return Require(transport, clock, cancellation, kline_plan, Sid27RequestSeedRequest(), 0x67,
                   "SID 27 (request seed) rejected", 10)
        .and_then(
            [&](const bytes::Bytes& seed_resp)
            {
                const bytes::Bytes seed(seed_resp.begin() + 6, seed_resp.begin() + 10);
                const bytes::Bytes seed_key = kline_plan.security == DensoSecurityVariant::kEcuTek
                                                  ? DensoSh705xKlineEcutekSeedKey(seed)
                                                  : DensoSh705xKlineStockSeedKey(seed);
                events.Log(LogLevel::kInfo, "Sending seed key to ECU");
                // Same positive-response code (0x67) as the request-seed step.
                return Require(transport, clock, cancellation, kline_plan, Sid27SendKeyRequest(seed_key), 0x67,
                               "SID 27 (send seed key) rejected");
            })
        .and_then(LogThen(events, "Set session mode",
                          [&]
                          {
                              return Require(transport, clock, cancellation, kline_plan, Sid10Request(), 0x50,
                                             "SID 10 (start diagnostic) rejected");
                          }))
        .transform(LogDone(events, "Successfully set to programming session"));
}

// The checksum-bypass 4-byte trailer after the kernel.
Status UploadChecksumBypass(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                            const DensoSh705xEepromKlinePlan& kline_plan, std::uint32_t address)
{
    return CheckCancelled(cancellation, "cancelled before checksum bypass")
        .and_then(
            [&]
            {
                return Require(transport, clock, cancellation, kline_plan, Sid34Request(address, 4), 0x74,
                               "checksum bypass request rejected");
            })
        .and_then(
            [&](const bytes::Bytes&)
            {
                const bytes::Bytes cks_bypass{0x00, 0x00, 0x5A, 0xA5};
                return TransferDataBlocks(transport, clock, cancellation, kline_plan.tester_id, kline_plan.target_id,
                                          address, DensoSh705xKlineEncryptPayload(cks_bypass), 4);
            });
}

// Up to 10 iterations, 200ms between attempts, 100ms extra
// settle once alive.
Status AwaitKernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                   IEventSink& events)
{
    events.Log(LogLevel::kInfo, "Requesting kernel ID...");
    for (int i = 0; i < kMaxKernelAlivePollIterations; ++i)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during kernel-alive poll");
        }
        Result<bytes::Bytes> poll = RequestKernelId(transport, clock, cancellation);
        if (!poll.has_value())
        {
            return std::unexpected(poll.error());
        }
        if (LooksKernelAlive(*poll))
        {
            if (Status slept = clock.Sleep(kKernelAliveSettleDelay, cancellation); !slept.has_value())
            {
                return slept;
            }
            events.Log(LogLevel::kInfo, "Kernel is alive");
            return {};
        }
        if (Status slept = clock.Sleep(kKernelPollRetryDelay, cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    return Fail(ErrorKind::kTimeout, "kernel did not respond after upload");
}

// One SID_DUMP request's raw reply: polled until `pagesize` bytes arrive or
// kMaxReadMemInnerRetries reads time out.
// `if (timeout >= 1000)` is legacy dead code -- the loop can only
// ever reach timeout==kMaxReadMemInnerRetries (5), so it never trips. A page
// that never fully arrives is therefore silently accepted short rather than
// erroring; preserved faithfully here (this is not the documented OOB-read
// fix this task authorizes -- that fix is scoped to kernel upload only).
Result<bytes::Bytes> ReadDumpPage(IKlineFlashTransport& transport, IClock& clock,
                                  const ICancellationToken& cancellation, bytes::ByteView request,
                                  std::uint32_t pagesize)
{
    if (Status requested =
            transport.Write(request)
                .and_then([&](std::size_t) { return clock.Sleep(kReadMemPreReadDelay, cancellation); })
                .and_then([&] { return CheckCancelled(cancellation, "cancelled after EEPROM request delay"); });
        !requested.has_value())
    {
        return std::unexpected(requested.error());
    }
    bytes::Bytes pagedata;
    int timeout = 0;
    while (pagedata.size() < pagesize && timeout < kMaxReadMemInnerRetries)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during EEPROM page read");
        }
        if (Status slept = clock.Sleep(kReadMemPollDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
        auto received = transport.Read(kShortTimeout, cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled after EEPROM page read");
        }
        if (received->has_value())
        {
            pagedata.append_range(**received);
        }
        else
        {
            ++timeout;
        }
    }
    return pagedata;
}

// Strip the [2-byte prefix][32 data][1 trailing] framing per
// block. Bounds-checked rather than mirroring Qt's clamping remove() calls,
// since pagedata can be short when a partial page gets through -- both
// approaches take "however much arrived" and neither indexes out of range.
void AppendDumpBlocks(bytes::Bytes& mapdata, bytes::ByteView pagedata, std::uint32_t numblocks)
{
    for (std::uint32_t block = 0; block < numblocks; ++block)
    {
        const std::size_t data_begin = static_cast<std::size_t>(block) * 35 + 2;
        if (data_begin >= pagedata.size())
        {
            return;
        }
        mapdata.append_range(
            pagedata.subspan(data_begin, std::min<std::size_t>(kEepromBlockBytes, pagedata.size() - data_begin)));
    }
}

} // namespace

Result<KlineConfig> DensoSh705xEepromKlineExecutor::Setup(const DensoSh705xEepromKlinePlan& kline_plan) const
{
    return NonIso14230KlineConfigFrom(kline_plan);
}

Result<FlashExecutionResult> DensoSh705xEepromKlineExecutor::Run(const DensoSh705xEepromKlinePlan& kline_plan,
                                                                 const FlashPlan& plan,
                                                                 IKlineFlashTransport& kline_transport, IClock& clock,
                                                                 const ICancellationToken& cancellation,
                                                                 IEventSink& events)
{

    // Force a known-good starting state for the driver's ISO14230 auto-header
    // flag before connect_bootloader()/upload_kernel(), which self-frame
    // every exchange via SsmProtocol::addHeader() and need this OFF to avoid
    // double-framing (legacy's implicit reliance on the default false,
    // commented out). Explicit here (rather than trusting the default)
    // because DesktopKlineFlashTransport's non-owning constructor wraps a
    // SerialPortActions instance that outlives a single execute() call
    // (MainWindow's session-lifetime serial member, shared across every
    // dialog attempt) -- a `true` left behind by an earlier attempt's
    // read_mem() phase (see below) must not leak into this attempt's
    // connect/upload phase.
    bool kernel_alive = false;
    if (Status connected = kline_transport.SetAddIso14230Header(false).and_then(
            [&] { return ConnectBootloader(kline_transport, clock, cancellation, events, kline_plan, kernel_alive); });
        !connected.has_value())
    {
        return std::unexpected(connected.error());
    }

    if (!kernel_alive)
    {
        // Safe to dereference: this executor only ever runs against
        // DensoSh705xEepromKline plans, and validate_and_build rejects any
        // plan for that family whose kernel is absent (flash_validation.cpp:
        // "family requires a kernel image"). check_family above confirms the
        // family tag; it does not itself guarantee a kernel --
        // validate_and_build is what does.
        if (Status uploaded =
                UploadKernel(kline_transport, clock, cancellation, events, kline_plan, plan.KernelOrEmpty());
            !uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
    }

    // read_mem()'s raw SID_DUMP request is NOT pre-framed by
    // SsmProtocol::addHeader() (unlike every connect/upload exchange above)
    // -- it needs the real serial driver to add the ISO14230 header itself,
    // which only happens with this flag ON. This is the exact regression
    // this call fixes: the legacy, now-deleted
    // EepromEcuSubaruDensoSH705xKlineOperation::read_mem() (commit 8ac6ba2)
    // called serial->set_add_iso14230_header(true) at exactly
    // this point, and the portable rewrite never called it at all.
    if (Status header_on = kline_transport.SetAddIso14230Header(true); !header_on.has_value())
    {
        return std::unexpected(header_on.error());
    }

    Result<bytes::Bytes> read_result =
        ReadMem(kline_transport, clock, cancellation, events, plan.TransferRegion(), kline_plan.mode);

    // Best-effort restore to the default OFF state: a
    // failure here must never mask read_mem()'s own result/error (it's
    // logged, not propagated), and it keeps the shared, session-lifetime
    // serial instance (non-owning transport case) from being left dirtied
    // for whatever unrelated K-Line operation the user runs next.
    if (Status header_off_after = kline_transport.SetAddIso14230Header(false); !header_off_after.has_value())
    {
        events.Log(LogLevel::kWarning, "failed to reset ISO14230 header mode after EEPROM read");
    }

    if (!read_result.has_value())
    {
        return std::unexpected(read_result.error());
    }

    return FlashExecutionResult{
        .operation = plan.Operation(),
        .read_bytes = std::move(*read_result),
    };
}

Status DensoSh705xEepromKlineExecutor::ConnectBootloader(IKlineFlashTransport& transport, IClock& clock,
                                                         const ICancellationToken& cancellation, IEventSink& events,
                                                         const DensoSh705xEepromKlinePlan& kline_plan,
                                                         bool& kernel_alive) const
{
    Result<bytes::Bytes> probe = CheckCancelled(cancellation, "cancelled before connect")
                                     .and_then(
                                         [&]
                                         {
                                             // Probe at bootloader speed first.
                                             return SetBaudAndSettle(transport, clock, cancellation, 62500,
                                                                     kProbeSettleDelay, "cancelled after probe delay");
                                         })
                                     .and_then(
                                         [&]
                                         {
                                             events.Log(LogLevel::kInfo, "Checking if kernel is already running...");
                                             return RequestKernelId(transport, clock, cancellation);
                                         });
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (LooksKernelAlive(*probe))
    {
        // Any well-formed alive response short-circuits the
        // rest of connect_bootloader() -- upload_kernel() is skipped too.
        kernel_alive = true;
        events.Log(LogLevel::kInfo, "Kernel already running");
        return {};
    }
    // BOTH "no frame at all" and "frame present but markers
    // wrong" fall through to the full init sequence below -- neither is a
    // hard failure at this point.
    events.Log(LogLevel::kWarning, "No response from kernel, initialising ECU...");
    // Drop back to 4800 baud for the SSM init handshake.
    return SetBaudAndSettle(transport, clock, cancellation, 4800, kInitSettleDelay, "cancelled after init delay")
        .and_then([&] { return InitProgrammingSession(transport, clock, cancellation, events, kline_plan); });
}

Status DensoSh705xEepromKlineExecutor::UploadKernel(IKlineFlashTransport& transport, IClock& clock,
                                                    const ICancellationToken& cancellation, IEventSink& events,
                                                    const DensoSh705xEepromKlinePlan& kline_plan,
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

    Result<DensoSh705xEepromUploadSizes> upload_sizes =
        ComputeDensoSh705xEepromUploadSizes(FlashFamily::kDensoSh705xEepromKline, kernel.bytes.size());
    if (!upload_sizes.has_value())
    {
        return std::unexpected(upload_sizes.error());
    }

    const std::uint32_t start_address = kernel.load_address;
    const std::uint32_t pl_len = upload_sizes->payload_bytes;

    // INTENTIONAL DEVIATION from legacy (documented per Global Constraints,
    // not a silent behavior change): legacy rounds pl_len UP but then calls
    // encrypt_payload() on the ORIGINAL, unpadded file buffer.
    // SsmProtocol::calculatePayload() silently clamps its output to
    // buf.size()&~3 whenever the requested len exceeds buf.size()
    // (ssm_protocol_core.cpp), and send_sid_36_transferdata() then
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

    // Change port speed to upload kernel.
    return transport.SetBaud(kline_plan.kernel_baud)
        .and_then(LogThen(events, "Requesting kernel upload",
                          [&]
                          {
                              return Require(transport, clock, cancellation, kline_plan,
                                             Sid34Request(start_address, pl_len), 0x74,
                                             "kernel upload request rejected");
                          }))
        .and_then(
            [&](const bytes::Bytes&)
            {
                const bytes::Bytes encrypted_kernel = DensoSh705xKlineEncryptPayload(padded_kernel);
                events.Log(LogLevel::kInfo, "Transfer kernel data");
                return TransferDataBlocks(transport, clock, cancellation, kline_plan.tester_id, kline_plan.target_id,
                                          start_address, encrypted_kernel, pl_len);
            })
        .and_then([&]
                  { return UploadChecksumBypass(transport, clock, cancellation, kline_plan, start_address + pl_len); })
        .and_then(
            [&]
            {
                events.Log(LogLevel::kInfo, "Kernel uploaded");
                // Jump to kernel. Cancellation is checked immediately
                // before this write per the portable seam's cancellation contract
                // ("before kernel jump").
                return CheckCancelled(cancellation, "cancelled before kernel jump");
            })
        .and_then(LogThen(events, "Jump to kernel",
                          [&]
                          {
                              return Require(transport, clock, cancellation, kline_plan, Sid31Request(), 0x71,
                                             "kernel start routine rejected");
                          }))
        .and_then(LogThen(events, "Kernel started, initializing...", [&] { return transport.SetBaud(62500); }))
        .and_then([&] { return AwaitKernel(transport, clock, cancellation, events); });
}

// read_mem() ("nisprog kernel" SID_DUMP protocol).
Result<bytes::Bytes> DensoSh705xEepromKlineExecutor::ReadMem(IKlineFlashTransport& transport, IClock& clock,
                                                             const ICancellationToken& cancellation, IEventSink& events,
                                                             const MemoryRegion& region, EepromReadMode mode) const
{
    const std::uint32_t start_addr = region.start;
    const std::uint32_t length = region.length;

    std::uint32_t skip_start = start_addr & (kEepromBlockBytes - 1);
    std::uint32_t addr = start_addr - skip_start;
    std::uint32_t willget = (skip_start + length + kEepromBlockBytes - 1) & ~(kEepromBlockBytes - 1);
    std::uint32_t len_done = 0;

    bytes::Bytes mapdata;
    events.Progress(0, static_cast<int>(length));

    while (willget != 0)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during EEPROM read");
        }

        const std::uint32_t numblocks = std::min(willget / kEepromBlockBytes, kNp10MaxBlocks);
        const std::uint32_t curblock = addr / kEepromBlockBytes;
        const std::uint32_t pagesize = numblocks * kEepromBlockBytes + numblocks * 3;

        const auto request = ComposeBe(kSidDump, bytes::Byte(mode), std::uint16_t(numblocks), std::uint16_t(curblock));
        Result<bytes::Bytes> pagedata = ReadDumpPage(transport, clock, cancellation, request, pagesize);
        if (!pagedata.has_value())
        {
            return std::unexpected(pagedata.error());
        }
        AppendDumpBlocks(mapdata, *pagedata, numblocks);

        std::uint32_t cplen = numblocks * kEepromBlockBytes - skip_start;
        skip_start = 0;

        if (Status slept = clock.Sleep(kReadMemInterBlockDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }

        if (const std::uint32_t extrabytes = cplen + len_done; extrabytes > length)
        {
            cplen -= (extrabytes - length);
        }
        len_done += cplen;
        addr += numblocks * kEepromBlockBytes;
        willget -= numblocks * kEepromBlockBytes;

        events.Progress(static_cast<int>(len_done), static_cast<int>(length));
    }

    events.Progress(static_cast<int>(length), static_cast<int>(length));
    return mapdata;
}

} // namespace fastecu::flash
