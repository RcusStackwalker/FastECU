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
// ISO-15765 CAN executors. The wave-4 consumers are
// SubaruDenso1n83m_1_5mCanExecutor, SubaruDensoSh72531CanExecutor,
// SubaruDensoSh72543CanDieselExecutor and SubaruDenso1n83m_4mCanExecutor.
// The wave-5 consumers are SubaruTcuDensoSh705xCanExecutor,
// SubaruDensoSh7058CanExecutor and SubaruDensoSh7058CanDieselExecutor.
//
// Each cluster was ported standalone, with duplication tolerated, so that
// factoring happened only after the finished executors and their independent
// characterization tests were visible. Direct comparison proved that the
// seed/key table, encrypt table and index transformation are byte-identical
// across all seven consumers. The three Wave-5 consumers use those shared
// values for the stock security flow and kernel upload; their normal-ROM
// BEEF payloads remain raw, so none consumes the decrypt table for ROM reads.
// The decrypt table remains valid for the Wave-4 normal-ROM read paths.
// DensoCAN is not a consumer of this data-only cluster.
//
// These applicable tables are the only shared artifact that is pure data.
// Everything else that looks alike -- the
// connect/probe shapes, the erase and reflash routines, the kernel jump, the
// stop and close-block retry loops, the checksum verify -- differs between
// the families in read timeouts, retry counts, pre-loop read counts, image
// base addresses, and, most importantly, in how strictly a bad response is
// treated. Those differences are the safety-relevant part of each family:
// collapsing them behind a parameterized common routine would make a future
// reader believe these are the same protocol when they are not. They stay in
// their own executors deliberately.
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
// payloads (and the applicable existing Wave-4 paths). Byte-identical across
// all seven consumers.
inline constexpr std::array<std::uint16_t, 4> kDensoIso15765EncryptTable{0xC85B, 0x32C0, 0xE282, 0x92A0};

// decrypt_payload's key-to-generate-index table, used by the applicable Wave-4
// normal-ROM dump paths. It is kDensoIso15765EncryptTable exactly reversed --
// calculatePayload's Feistel structure inverts by reversing key order -- but
// it is spelled out rather than derived, because that is how the applicable
// legacy sources spell it and a derived table would hide a future divergence.
// No Wave-5 normal-ROM BEEF path consumes this table.
inline constexpr std::array<std::uint16_t, 4> kDensoIso15765DecryptTable{0x92A0, 0xE282, 0x32C0, 0xC85B};

// The three SsmProtocol calls that bind the tables above. All four cluster
// members and the applicable Wave-5 families carried these byte-for-byte;
// they are pure table-to-algorithm adapters with no protocol sequence in them.

// generate_can_seed_key().
inline bytes::Bytes denso_seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kDensoIso15765SeedKeyTable, SsmProtocol::kIndexTransformationStock);
}

// encrypt_payload(), run once over the whole image before a flash write.
inline bytes::Bytes denso_encrypt_rom(bytes::ByteView image)
{
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()), kDensoIso15765EncryptTable,
                                         SsmProtocol::kIndexTransformationStock);
}

// decrypt_payload(), run per 256-byte page as a dump arrives.
inline bytes::Bytes denso_decrypt_page(bytes::ByteView page)
{
    return SsmProtocol::calculatePayload(page, static_cast<std::uint32_t>(page.size()), kDensoIso15765DecryptTable,
                                         SsmProtocol::kIndexTransformationStock);
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

} // namespace fastecu::flash
