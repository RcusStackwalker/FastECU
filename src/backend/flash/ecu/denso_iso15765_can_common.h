#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// The SSM seed/key and payload crypto constants shared by the Denso
// ISO-15765 CAN executors. The bootloader-dialect consumers are
// SubaruDenso1n83m_1_5mCanExecutor, SubaruDensoSh72531CanExecutor,
// SubaruDensoSh72543CanDieselExecutor and SubaruDenso1n83m_4mCanExecutor.
// The BEEF-dialect consumers are SubaruTcuDensoSh705xCanExecutor,
// SubaruDensoSh7058CanExecutor and SubaruDensoSh7058CanDieselExecutor.
//
// Each cluster was ported standalone, with duplication tolerated, so that
// factoring happened only after the finished executors and their independent
// characterization tests were visible. Direct comparison proved that the
// seed/key table, encrypt table and index transformation are byte-identical
// across all seven consumers. The three BEEF-dialect consumers use those shared
// values for the stock security flow and kernel upload; their normal-ROM
// BEEF payloads remain raw, so none consumes the decrypt table for ROM reads.
// The decrypt table remains valid for the bootloader-dialect normal-ROM read paths.
// DensoCAN is not a consumer of this data-only cluster.
//
// Beyond these tables, the common module shares thirteen narrow operations: pure
// setup-PDU encoding, the SecurityAccess seed/key exchange, the erase flow,
// one encrypted B7 page read and the read dump built on it (page loop, stop and
// padding), one B6 write request and the image write loop built on it, best-effort transfer
// exit and the settle-and-checksum close around it (all four Denso ISO-15765 bootloader dialect executors), and the
// N83M in-car opening exchange run (the two N83M executors only; its 0x7E1 request asks for session 0x63, where the
// SH-family runs send 0x03), plus the kernel jump with its continuation wait (all four; the read timeouts and the N83M
// 4M/SH72531 bench discard are parameters), and connect_bootloader's prelude (OBK probe, identity queries,
// access-method probe and branch selector), which direct comparison showed byte-identical across all four apart from
// one log line, the diesel ECU ID query and the diesel probe timeout, all kept as parameters or local. The single-block
// write_memory body is shared too; each family's reflash is now only its header log line, the shared write loop and the
// shared close. Everything else that looks alike -- the in-car and bench connect arms,
// -- differs between the
// families in read timeouts, retry counts, pre-loop read counts, image base addresses, and, most importantly, in how
// strictly a bad response is treated. Those differences are the safety-relevant part of each family: collapsing them
// behind a parameterized common routine would make a future reader believe these are the same protocol when they are
// not. They stay in their own executors deliberately.
//
// Scope note: the index transformation these tables are paired with is NOT
// here. It is not specific to this cluster -- it was spelled out at fourteen
// production call sites across this package and src/backend/flash/eeprom --
// and now lives in src/algorithms/protocol/ssm as
// SsmProtocol::kIndexTransformationStock, next to the ECUTEK variant it is
// nearly identical to. Only the key-to-generate-index tables below, which
// are genuinely per-family protocol data, remain here.
//
// The executor test suites do NOT read these constants back: each carries
// literal wire expectations transcribed independently from its legacy oracle,
// so a wrong entry here fails those suites rather than passing silently. Keep
// it that way.

// generate_can_seed_key's key-to-generate-index table (legacy
// flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp lines 1474-1479 and the
// corresponding lines of the SH72531, SH72543 diesel and 1N83M 4M sources),
// confirmed byte-identical across all seven consumers by direct comparison.
inline constexpr std::array<std::uint16_t, 16> kDensoIso15765SeedKeyTable{
    0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
    0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};

// encrypt_payload's key-to-generate-index table, used for padded kernel-upload
// payloads (and the applicable existing bootloader-dialect paths). Byte-identical across
// all seven consumers.
inline constexpr std::array<std::uint16_t, 4> kDensoIso15765EncryptTable{0xC85B, 0x32C0, 0xE282, 0x92A0};

// decrypt_payload's key-to-generate-index table, used by the applicable bootloader-dialect
// normal-ROM dump paths. It is kDensoIso15765EncryptTable exactly reversed --
// calculatePayload's Feistel structure inverts by reversing key order -- but
// it is spelled out rather than derived, because that is how the applicable
// legacy sources spell it and a derived table would hide a future divergence.
// No BEEF-dialect normal-ROM BEEF path consumes this table.
inline constexpr std::array<std::uint16_t, 4> kDensoIso15765DecryptTable{0x92A0, 0xE282, 0x32C0, 0xC85B};

// The three SsmProtocol calls that bind the tables above. All four cluster
// members and the applicable BEEF-dialect families carried these byte-for-byte;
// they are pure table-to-algorithm adapters with no protocol sequence in them.

// generate_can_seed_key().
inline bytes::Bytes DensoSeedKey(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kDensoIso15765SeedKeyTable, ssm_protocol::kIndexTransformationStock);
}

// encrypt_payload(), run once over the whole image before a flash write.
inline bytes::Bytes DensoEncryptRom(bytes::ByteView image)
{
    return ssm_protocol::CalculatePayload(image, static_cast<std::uint32_t>(image.size()), kDensoIso15765EncryptTable,
                                          ssm_protocol::kIndexTransformationStock);
}

// decrypt_payload(), run per 256-byte page as a dump arrives.
inline bytes::Bytes DensoDecryptPage(bytes::ByteView page)
{
    return ssm_protocol::CalculatePayload(page, static_cast<std::uint32_t>(page.size()), kDensoIso15765DecryptTable,
                                          ssm_protocol::kIndexTransformationStock);
}

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
// the SH families pace each page by 1 ms (`pace_pages`). Setup stays local:
// its policy and tolerance differ per family.
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

// Sends `pdu`, reads one reply, and compares its first two bytes. A mismatch
// is LOGGED AND TOLERATED -- the frame is still returned and the caller
// decides -- which is what legacy does at these sites; only an absent or
// too-short reply is fatal. Callers that read further into the returned frame
// must length-check it themselves: this only guarantees two bytes.
//
// `timeout` is a parameter, not a cluster constant: three members probe with
// their short timeout and subaru_denso_sh72543_can_diesel probes with its long
// one. Collapsing that difference is exactly what this file's header comment
// warns against.
//
// Not specific to this cluster in principle, but shared here because these
// four are the families that provably share it.
Result<bytes::Bytes> TolerantProbe(const CanExecutorContext& ctx, bytes::ByteView pdu, bytes::Byte expected_service,
                                   bytes::Byte expected_subfunction, std::chrono::milliseconds timeout,
                                   std::string_view rejection_prefix, std::string_view subject);

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

// Sends `pdu` to `request_id` and reads one reply only to discard it: the
// in-car arm's opening run of session/DTC/communication-control exchanges
// whose answers legacy never inspects. The reply is read from `can` directly,
// so the channel's response id is never consulted.
Status FireAndForget(const CanExecutorContext& ctx, ICanFlashTransport& can, std::uint32_t request_id,
                     bytes::ByteView pdu, std::chrono::milliseconds timeout);

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
// each read at 200 ms and discarded. Its 0x7E1 request asks for session 0x63;
// the SH72531 and SH72543 diesel in-car arms send 0x03 there and keep their own
// runs.
Status N83mInCarFireAndForget(const CanExecutorContext& ctx, ICanFlashTransport& can);

// write_memory's body: each family erases and reflashes exactly one block
// (legacy block_modified marks one entry), so this logs `block_index` -- 1 of
// a three-block table on the N83M and SH72531 families, 0 of SH72543 diesel's
// one-block table -- erases `block` with DensoIso15765Erase, then runs the
// family's `reflash(PhaseReporter&)` under a "Write ROM" phase sized to the
// block. Each family's reflash logs its own header line around the shared write and close.
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
