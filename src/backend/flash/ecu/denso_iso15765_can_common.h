#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/flash_executor.h"
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
// Beyond these tables, the common module shares three narrow operations: the
// SecurityAccess seed/key exchange and the erase flow (all four
// Denso ISO-15765 bootloader dialect executors) and the N83M in-car opening exchange run (the two N83M executors
// only; its 0x7E1 request asks for session 0x63, where the SH-family runs send
// 0x03). Everything else that looks alike -- the connect/probe shapes, the
// reflash routines, the kernel jump, the stop and close-block retry loops, the
// checksum verify -- differs between the families in read timeouts, retry
// counts, pre-loop read counts, image base addresses, and, most importantly, in
// how strictly a bad response is treated. Those differences are the
// safety-relevant part of each family: collapsing them behind a parameterized
// common routine would make a future reader believe these are the same
// protocol when they are not. They stay in their own executors deliberately.
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
inline bytes::Bytes denso_seed_key(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kDensoIso15765SeedKeyTable, ssm_protocol::kIndexTransformationStock);
}

// encrypt_payload(), run once over the whole image before a flash write.
inline bytes::Bytes denso_encrypt_rom(bytes::ByteView image)
{
    return ssm_protocol::CalculatePayload(image, static_cast<std::uint32_t>(image.size()), kDensoIso15765EncryptTable,
                                          ssm_protocol::kIndexTransformationStock);
}

// decrypt_payload(), run per 256-byte page as a dump arrives.
inline bytes::Bytes denso_decrypt_page(bytes::ByteView page)
{
    return ssm_protocol::CalculatePayload(page, static_cast<std::uint32_t>(page.size()), kDensoIso15765DecryptTable,
                                          ssm_protocol::kIndexTransformationStock);
}

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
Result<bytes::Bytes> tolerant_probe(const CanExecutorContext& ctx, bytes::ByteView pdu, bytes::Byte expected_service,
                                    bytes::Byte expected_subfunction, std::chrono::milliseconds timeout,
                                    std::string_view rejection_prefix, std::string_view subject);

// Sends `pdu` to `request_id` and reads one reply only to discard it: the
// in-car arm's opening run of session/DTC/communication-control exchanges
// whose answers legacy never inspects. The reply is read from `can` directly,
// so the channel's response id is never consulted.
Status fire_and_forget(const CanExecutorContext& ctx, ICanFlashTransport& can, std::uint32_t request_id,
                       bytes::ByteView pdu, std::chrono::milliseconds timeout);

// The bootloader dialect's SecurityAccess exchange: request the level-0x61 seed, derive the
// key from its four payload bytes with denso_seed_key, send it at level 0x62.
// Both exchanges are fatal on a rejected, absent or mismatched reply, and a
// seed reply with fewer than four seed bytes fails before any key is sent.
// Both read with a single 2000 ms read_timeout; pending replies keep the UDS
// client's own pending timeout and are re-read, never re-sent.
Status denso_security_access(const CanExecutorContext& ctx);

// The bootloader dialect's flash erase: RequestDownload with `request_download_setup_pdu`
// (the executor builds it, since it also builds the read-path setup), then the
// erase routine trigger, then up to twenty re-reads for the 71 01 02 success
// reply. The trigger is sent once and never re-sent while polling. The setup
// reply must start 20 01 05 or the trigger is never sent. Setup and polling
// both read at 500 ms, with a 500 ms sleep after the trigger and after each
// unsuccessful poll.
Status denso_iso15765_erase(const CanExecutorContext& ctx, bytes::ByteView request_download_setup_pdu);

// The N83M 1.5M and 4M in-car arms' opening run of ten session, DTC and
// communication-control requests across 0x7A2, 0x7E0, 0x7DF, 0x7E1 and 0x7B0,
// each read at 200 ms and discarded. Its 0x7E1 request asks for session 0x63;
// the SH72531 and SH72543 diesel in-car arms send 0x03 there and keep their own
// runs.
Status n83m_in_car_fire_and_forget(const CanExecutorContext& ctx, ICanFlashTransport& can);

} // namespace fastecu::flash
