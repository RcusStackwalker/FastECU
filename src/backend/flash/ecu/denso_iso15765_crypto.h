#pragma once

#include <array>
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::flash
{

// The SSM seed/key and payload crypto constants shared by the Denso
// ISO-15765 CAN executors. The bootloader-dialect consumer is
// DensoIso15765BootloaderExecutor, for the N83M 1.5M, N83M 4M, SH72531 and
// SH72543 diesel families. The BEEF-dialect consumers are SubaruTcuDensoSh705xCanExecutor,
// SubaruDensoSh7058CanExecutor and SubaruDensoSh7058CanDieselExecutor.
//
// Each family was ported standalone, with duplication tolerated, so that
// factoring happened only after the finished executors and their independent
// characterization tests were visible. Direct comparison proved that the
// seed/key table, encrypt table and index transformation are byte-identical
// across all seven consumers. The three BEEF-dialect consumers use those shared
// values for the stock security flow and kernel upload; their normal-ROM
// BEEF payloads remain raw, so none consumes the decrypt table for ROM reads.
// The decrypt table remains valid for the bootloader-dialect normal-ROM read paths.
// DensoCAN is not a consumer of this data-only module.
//
// Scope note: the index transformation these tables are paired with is NOT
// here. It is not specific to these tables -- it was spelled out at fourteen
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
// flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp and the SH72531,
// SH72543 diesel and 1N83M 4M sources), confirmed
// byte-identical across all seven consumers by direct comparison.
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

// The three SsmProtocol calls that bind the tables above. All four bootloader-dialect
// families and the applicable BEEF-dialect families carried these byte-for-byte;
// they are pure table-to-algorithm adapters with no protocol sequence in them.

// generate_can_seed_key().
inline bytes::Bytes DensoSeedKey(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kDensoIso15765SeedKeyTable, ssm_protocol::kStockPairTable);
}

// encrypt_payload(), run once over the whole image before a flash write.
inline bytes::Bytes DensoEncryptRom(bytes::ByteView image)
{
    return ssm_protocol::CalculatePayload(image, kDensoIso15765EncryptTable, ssm_protocol::kStockPairTable);
}

// decrypt_payload(), run per 256-byte page as a dump arrives.
inline bytes::Bytes DensoDecryptPage(bytes::ByteView page)
{
    return ssm_protocol::CalculatePayload(page, kDensoIso15765DecryptTable, ssm_protocol::kStockPairTable);
}

} // namespace fastecu::flash
