#pragma once

#include <array>
#include <cstdint>

namespace fastecu::flash
{

// The SSM seed/key and payload crypto constants shared by the wave-4 Denso
// ISO-15765 CAN cluster: SubaruDenso1n83m_1_5mCanExecutor,
// SubaruDensoSh72531CanExecutor, SubaruDensoSh72543CanDieselExecutor and
// SubaruDenso1n83m_4mCanExecutor.
//
// Each of those four was ported standalone, with duplication tolerated, so
// that the factoring decision could be made once with all four visible. That
// pass (the wave-4 cluster-factoring task) compared the finished executors
// block by block. These four tables are the ONLY four-way byte-identical
// artifact that is pure data. Everything else that looks alike -- the
// connect/probe shapes, the erase and reflash routines, the kernel jump, the
// stop and close-block retry loops, the checksum verify -- differs between
// the families in read timeouts, retry counts, pre-loop read counts, image
// base addresses, and, most importantly, in how strictly a bad response is
// treated. Those differences are the safety-relevant part of each family:
// collapsing them behind a parameterized common routine would make a future
// reader believe these are the same protocol when they are not. They stay in
// their own executors deliberately. See
// docs/superpowers/specs/2026-08-22-step5-tail-wave4-denso-iso15765-can-design.md.
//
// Scope note: the 32-entry index transformation below is not specific to this
// cluster -- the same table appears verbatim in four further executors in this
// package (subaru_hitachi_m32r_can, subaru_tcu_cvt_hitachi_m32r_can,
// subaru_tcu_cvt_mitsu_mh8104_can, subaru_tcu_cvt_mitsu_mh8111_can), which
// keep their own copies. It is an SsmProtocol-level constant wearing a
// cluster-level name here. Promoting it to src/algorithms/protocol/ssm and
// retiring all eight copies is a separate, package-wide change and is
// deliberately out of this wave's scope.
//
// The four executors' own test suites do NOT read these constants back: each
// test file carries its own copy, transcribed independently from the same
// legacy lines, so a wrong entry here fails those suites rather than passing
// silently. Keep it that way.

// generate_can_seed_key's key-to-generate-index table (legacy
// flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp lines 1474-1479 and the
// corresponding lines of the SH72531, SH72543 diesel and 1N83M 4M sources),
// confirmed byte-identical across all four by direct comparison.
inline constexpr std::array<std::uint16_t, 16> kDensoIso15765SeedKeyTable{
    0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
    0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};

// encrypt_payload's key-to-generate-index table, used for the whole-ROM
// encrypt that precedes a flash write. Byte-identical across all four.
inline constexpr std::array<std::uint16_t, 4> kDensoIso15765EncryptTable{0xC85B, 0x32C0, 0xE282, 0x92A0};

// decrypt_payload's key-to-generate-index table, used on each 256-byte page of
// a dump. It is kDensoIso15765EncryptTable exactly reversed -- calculatePayload's
// Feistel structure inverts by reversing key order -- but it is spelled out
// rather than derived, because that is how all four legacy sources spell it
// and a derived table would hide a future divergence.
inline constexpr std::array<std::uint16_t, 4> kDensoIso15765DecryptTable{0x92A0, 0xE282, 0x32C0, 0xC85B};

// The index transformation both calculateSeedKey and calculatePayload take.
// See the scope note above: shared here across the four cluster members only.
inline constexpr std::array<std::uint8_t, 32> kDensoIso15765IndexTransformation{
    0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
    0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

} // namespace fastecu::flash
