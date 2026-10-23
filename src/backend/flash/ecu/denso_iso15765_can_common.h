#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/denso_iso15765_bootloader_profile.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// The dialect's rejection log prefix, legacy's "Wrong response from ECU: ".
inline constexpr std::string_view kDensoIso15765RejectionPrefix = "Wrong response from ECU: ";

// The common module shares eleven operations across the four
// bootloader-dialect families (the seed/key and payload tables they use are
// in denso_iso15765_crypto.h):
// - setup-PDU encoding;
// - the SecurityAccess seed/key exchange;
// - the erase flow;
// - the read dump: B7 page reads, the best-effort stop and the 0xFF padding;
// - the image write loop: B6 chunk writes at the family's image base;
// - the close: best-effort transfer exit, settle and checksum verify;
// - the single-block write_memory body around the family's reflash;
// - connect_bootloader's prelude: OBK probe, identity queries, access-method
//   probe and branch selector;
// - the in-car arm, with one fire-and-forget run for the N83M families and one
//   for the SH families;
// - the bench arm;
// - the kernel jump with its continuation wait.
// Each declaration below names the family differences it takes as parameters.
// Direct comparison of the legacy sources proved everything else identical.
// What still differs -- read timeouts, retry counts and, above all, how
// strictly a bad response is treated -- is the safety-relevant part of each
// family. DensoIso15765BootloaderExecutor reads it from the family's dialect
// profile (denso_iso15765_bootloader_profile.h), where each value carries its
// legacy citation.
//
// Most exchanges go through UdsClient over CanFlashUdsChannel. The
// exceptions here are the probes and fire-and-forget writes whose reply legacy
// either tolerates or never inspects at all: UdsClient enforces the
// SID+0x40 convention and decodes NRCs, so those go through the channel --
// or, where legacy does not even validate the reply's arbitration id,
// through the transport -- directly. `channel` is the same
// CanFlashUdsChannel instance `uds` wraps.
//
// Executors that log their own rejections use kDensoIso15765RejectionPrefix.
//
// Pure setup encoding shared by the N83M 1.5M/4M, SH72531 and SH72543 diesel
// bootloader executors: RequestDownload (34) or RequestUpload (35), 04 44,
// then four-byte big-endian start and length. Region fields pass through
// unchanged; callers own validation and policy.
bytes::Bytes DensoIso15765DownloadSetupPdu(const MemoryRegion& region);
bytes::Bytes DensoIso15765UploadSetupPdu(const MemoryRegion& region);

// One B7 request with a four-byte big-endian address, fatal UDS response
// handling at 2000 ms, and whole-word payload decryption. Short pages remain
// accepted; DensoIso15765ReadDump below owns the loop around it.
Result<bytes::Bytes> ReadDensoEncryptedPage(const CanExecutorContext& ctx, std::uint32_t address);

// read_memory after its setup exchanges: one B7 page per 256 bytes of
// `region`, cancellation checked before each, progress after each; then up to
// six best-effort 37 stop requests at 500 ms, whose outcome is never checked;
// then the dump between `lead_pad_len` and `tail_pad_len` bytes of 0xFF. Only
// the SH families pace each page by 1 ms (`pace_pages`). The setup exchanges
// before it are the dialect executor's: their policy and tolerance differ per
// family.
Result<bytes::Bytes> DensoIso15765ReadDump(const CanExecutorContext& ctx, const MemoryRegion& region,
                                           std::size_t lead_pad_len, std::size_t tail_pad_len, PhaseReporter& progress,
                                           bool pace_pages);

// One fatal B6 request at 500 ms with an absolute wire address and the
// caller-selected encrypted bytes. DensoIso15765WriteImage below owns the loop.
Status DensoIso15765WriteChunk(const CanExecutorContext& ctx, std::uint32_t address, bytes::ByteView encrypted_chunk);

// reflash_block's write loop: the whole image is encrypted once, then one B6
// chunk per whole 256 bytes of `block`, cancellation checked before each and
// progress after each. Chunk bytes are taken at the wire address minus
// `image_base`, the flash address of the image's byte 0 (nonzero on both N83M
// families). DensoIso15765WriteImageAndVerifyChecksum adds the close.
Status DensoIso15765WriteImage(const CanExecutorContext& ctx, bytes::ByteView image, std::uint32_t image_base,
                               const MemoryRegion& block, PhaseReporter& progress);

// Up to six re-sent 37 requests at 200 ms, swallowing every returned error.
// Logs the first positive reply and stops; callers own settle and checksum.
void DensoIso15765BestEffortTransferExit(const CanExecutorContext& ctx);

// reflash_block's close: best-effort transfer exit, a 100 ms settle, then the
// fatal checksum-verify routine 31 01 02 02 01 expecting 71 01 02. The read
// timeouts are the family's own `checksum_policy` (500/500 on N83M 1.5M,
// 500/3000 on N83M 4M, 2000/2000 on both SH families).
Status DensoIso15765CloseAndVerifyChecksum(const CanExecutorContext& ctx, const uds::ExchangePolicy& checksum_policy);

// reflash_block after the family's own header log line: the image write loop,
// then the close. A failed or cancelled write never reaches the close.
Status DensoIso15765WriteImageAndVerifyChecksum(const CanExecutorContext& ctx, bytes::ByteView image,
                                                std::uint32_t image_base, const MemoryRegion& block,
                                                PhaseReporter& progress, const uds::ExchangePolicy& checksum_policy);

// fatal_query with this dialect's "Wrong response from ECU: " rejection
// prefix; see uds_client_exchange_common.h for what expected_prefix, subject
// and min_payload_size mean.
Result<bytes::Bytes> FatalQuery(const CanExecutorContext& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                                const uds::ExchangePolicy& policy, std::string_view subject,
                                std::optional<std::size_t> min_payload_size = std::nullopt);

// The kernel jump's continuation: check the current reply for 50 <session>,
// sleep 100 ms, receive at `receive_timeout`. The last receive remains
// unchecked; exhaustion logs and succeeds.
Status DensoIso15765WaitForJumpAck(const CanExecutorContext& ctx, bytes::Byte session,
                                   std::optional<bytes::Bytes> first_reply, int attempt_count,
                                   std::chrono::milliseconds receive_timeout);

// connect_bootloader's kernel jump: send 10 <session>, read one reply at
// `first_read_timeout`, then the continuation above at `loop_read_timeout`
// for at most `max_tries` (10 in car, 50 on the bench). The N83M 4M and
// SH72531 bench arms set `discard_first_reply`: a 50 ms wait and a second
// read at `first_read_timeout` replace even a matching first reply. Every
// family reads at 200 ms except SH72543 diesel, which reads its first reply at
// 2000 ms and continues at 2000 ms in car and 200 ms on the bench.
Status DensoIso15765JumpToKernel(const CanExecutorContext& ctx, bytes::Byte session, int max_tries,
                                 std::chrono::milliseconds first_read_timeout,
                                 std::chrono::milliseconds loop_read_timeout, bool discard_first_reply);

// connect_bootloader's OBK-already-active probe: 10 5F through the channel,
// one read at 200 ms. True when the reply starts 50 5F, after logging
// `active_log` (the families differ only in that wording). A mismatch, an
// absent frame or an empty PDU is a non-fatal miss that logs and returns
// false; only send and receive errors fail.
Result<bool> DensoObkIsActive(const CanExecutorContext& ctx, std::string_view active_log);

// connect_bootloader's identity prelude: the ECU ID query (AA; SH72543 diesel
// reads it by DID instead and keeps that query local), then VIN, CAL ID and
// CVN (09 02/04/06). Each logs its request, reads at 2000 ms and is non-fatal:
// a mismatch or an exchange error, cancellation included, is logged and
// swallowed.
void DensoEcuIdQuery(const CanExecutorContext& ctx);
void DensoVehicleInfoQueries(const CanExecutorContext& ctx);

// connect_bootloader's access-method probe (10 5F, expecting 50 01) and
// programming-branch selector (22 10 1D, expecting 62 10), both tolerant at
// `probe_timeout`. True selects the in-car arm: selector payload byte 3 is not
// 0xFF. A selector reply shorter than four bytes fails rather than reading out
// of bounds, which legacy would do.
Result<bool> DensoProgrammingBranchIsInCar(const CanExecutorContext& ctx, std::chrono::milliseconds probe_timeout);

// The bootloader dialect's SecurityAccess exchange: request the level-0x61 seed, derive the
// key from its four payload bytes with denso_seed_key, send it at level 0x62.
// Both exchanges are fatal on a rejected, absent or mismatched reply, and a
// seed reply with fewer than four seed bytes fails before any key is sent.
// Both read with a single 2000 ms read_timeout; pending replies keep the UDS
// client's own pending timeout and are re-read, never re-sent.
Status DensoSecurityAccess(const CanExecutorContext& ctx);

// The bootloader dialect's flash erase: builds RequestDownload setup from the
// caller-selected region with DensoIso15765DownloadSetupPdu, then sends the
// erase routine trigger, then up to twenty re-reads for the 71 01 02 success
// reply. The trigger is sent once and never re-sent while polling. The setup
// reply must start 20 01 05 or the trigger is never sent. Setup and polling
// both read at 500 ms, with a 500 ms sleep after the trigger and after each
// unsuccessful poll.
Status DensoIso15765Erase(const CanExecutorContext& ctx, const MemoryRegion& region);

// The N83M 1.5M and 4M in-car arms' opening run of ten session, DTC and
// communication-control requests across 0x7A2, 0x7E0, 0x7DF, 0x7E1 and 0x7B0,
// each read at 200 ms and discarded. Its 0x7E1 request asks for session 0x63.
Status N83mInCarFireAndForget(const CanExecutorContext& ctx, ICanFlashTransport& can);

// The SH72531 and SH72543 diesel in-car arms' run: the same ten requests but
// session 0x03 on 0x7E1. The 0x7E0 session-open reply reads at
// `primary_timeout` (200 ms on SH72531, 2000 ms on the diesel); the rest at
// 200 ms.
Status ShInCarFireAndForget(const CanExecutorContext& ctx, ICanFlashTransport& can,
                            std::chrono::milliseconds primary_timeout);

// connect_bootloader's in-car programming arm: a 500 ms wait, the tolerant
// access-method probe (10 5F, expecting 50 01), the family's fire-and-forget
// run, seed/key, the fatal session confirmation (10 5F, expecting 50 63) and
// programming-mode re-read (22 10 1D, expecting 62 10), then the kernel jump
// (10 62, at most 10 tries). Every read in the arm -- the probe, both
// confirmations, the run's 0x7E0 reply and the jump -- uses `read_timeout`:
// 200 ms, or 2000 ms on SH72543 diesel.
Status DensoIso15765ConnectInCar(const CanExecutorContext& ctx, ICanFlashTransport& can, DensoInCarRun run,
                                 std::chrono::milliseconds read_timeout);

// connect_bootloader's bench programming arm: the arm's settle, the bench
// session (10 43, expecting 50 43) read at `session_timeout` -- fatal, or a
// tolerant probe on SH72543 diesel -- seed/key, then the bench kernel jump
// (10 42, at most 50 tries) read at the arm's jump timeouts.
Status DensoIso15765ConnectBench(const CanExecutorContext& ctx, const DensoBenchArm& arm);

// write_memory's body: each family erases and reflashes exactly one block
// (legacy block_modified marks one entry), so this logs `block_index` -- 1 of
// a three-block table on the N83M and SH72531 families, 0 of SH72543 diesel's
// one-block table -- erases `block` with DensoIso15765Erase, then runs the
// family's `reflash(PhaseReporter&)` under a "Write ROM" phase sized to the
// block. The dialect executor's reflash logs the family's header line around
// the shared write and close.
template <typename Reflash>
Status DensoIso15765WriteBlock(const CanExecutorContext& ctx, const MemoryRegion& block, int block_index,
                               PhaseSequence& phases, Reflash&& reflash)
{
    LogInfo(ctx, std::format("Blocks to flash: {},  (total: 1)", block_index));
    LogInfo(ctx, "--- Erasing ECU flash memory ---");
    PhaseReporter erase = phases.Start("Erase", 1);
    if (const Status erased = DensoIso15765Erase(ctx, block); !erased.has_value())
    {
        return erased;
    }
    erase.Complete();

    LogInfo(ctx, "--- Start writing ROM file to ECU flash memory ---");
    PhaseReporter write = phases.Start("Write ROM", static_cast<int>(block.length));
    if (const Status written = reflash(write); !written.has_value())
    {
        return written;
    }
    write.Complete();
    LogInfo(ctx, std::format("Block {} reflash complete.", block_index));
    return {};
}

} // namespace fastecu::flash
