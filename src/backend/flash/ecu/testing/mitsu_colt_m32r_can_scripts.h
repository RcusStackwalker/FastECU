#pragma once
// Wire scripts for MitsuColtM32rCanExecutor, shared by its executor test and
// the transcript recorder so both drive the executor with the same exchanges.
#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash::colt_testing
{

inline constexpr std::string_view kProtocol384 = "mitsu_ecu_m32r_can";
inline constexpr std::string_view kVendorProtocol384 = "mitsu_ecu_m32r_can_vendor_ext";
inline constexpr std::string_view kProtocol512 = "mitsu_ecu_m32r_can_512kb";
inline constexpr std::string_view kMcu384 = "M32R_384KB_1block";
inline constexpr std::string_view kMcu512 = "M32R_512KB_1block";

// The 4 seed bytes the ECU returns in the scripts below. Deliberately all
// distinct and distinct from the surrounding framing bytes, so that reading
// the seed from any offset other than the legacy `received.mid(6, 4)` yields
// a different seed -- and therefore a different key on the wire, which the
// scripted transport rejects.
inline constexpr auto kSeed = std::to_array<bytes::Byte>({0x11, 0x22, 0x33, 0x44});

std::string_view McuFor(std::string_view protocol);

// Every request on this bus carries a 4-byte big-endian 0x7E0 prefix
// (legacy build_request, flash_ecu_mitsu_m32r_can_operation.cpp:58-64).
bytes::Bytes Request(bytes::ByteView payload);

// Responses carry the 4-byte 0x7E8 reply id; the legacy code indexes
// received.at(4) for the service byte throughout.
bytes::Bytes Response(bytes::ByteView tail);
bytes::Bytes Response(std::initializer_list<bytes::Byte> tail);

Result<FlashPlan> ReadPlan(std::string_view protocol = kProtocol384);

// A Write plan is what selects kSessionBootload, and kSessionBootload is the
// only thing that reaches connect_bootloader()'s factory SecurityAccess arm.
// This helper defaults to the 512 KiB protocol, whose builder declares both
// ConfirmationSpecs, so a plan from here is fully gated.
Result<FlashPlan> WritePlan(bytes::Bytes rom, std::string_view protocol = kProtocol512);

// The two checksum bytes the executor must commit, in order, for the
// userspace slice of the ROM WriteRom() builds. Written out rather than
// recomputed with checksum(): the scripted CRC frame is the only place the
// executor's own running sum and byte split are observable, so deriving the
// expectation the way the implementation does would let an inversion pass in
// both.
inline const bytes::Bytes kUserspaceChecksumBytes{0x12, 0x34};

// The ROM image every 512 KiB write test writes: kTopRegionEnd bytes, 0x00
// below kTopRegionStart and 0xEE above it. The distinctive top-region fill is
// what makes the bootstrap comparison observable -- an ECU that reports 0xEE
// matches, one that reports 0xFF (erased flash) does not.
//
// The userspace window additionally opens with 18 * 0xFF followed by 0x46,
// which sums to exactly 0x1234 (18 * 255 + 70) over the otherwise-zero
// slice -- a checksum whose two halves differ, so the order they go on the
// wire in is pinned too (kUserspaceChecksumBytes).
bytes::Bytes WriteRom();

// The 384 KiB counterpart of WriteRom(), with the same userspace contents.
bytes::Bytes WriteRom384();

// The userspace slice of `rom` the write path must transfer.
bytes::ByteView UserspaceOf(const bytes::Bytes& rom);

// The top-region slice of `rom` the bootstrap must write and verify.
bytes::ByteView TopRegionOf(const bytes::Bytes& rom);

// The ECU memory ScriptAddressMarkedRead serves: each byte is the low byte of
// its own address.
bytes::Bytes AddressMarkedImage(std::uint32_t length);

// Scripts the chunked ReadMemoryByAddress sweep over [start, start+length),
// filling every payload with `fill`.
void ScriptFlashRead(ScriptedCanFlashTransport& transport, std::uint32_t start, std::uint32_t length, bytes::Byte fill);

// Scripts a single ReadMemoryByAddress chunk at `addr`, replying with
// `chunk_len` bytes of `fill`. Used where the caller wants exactly one
// chunk on the wire instead of ScriptFlashRead's full-range sweep -- e.g.
// to prove top_region_matches() stops issuing reads at the first mismatch.
void ScriptFlashReadChunk(ScriptedCanFlashTransport& transport, std::uint32_t addr, bytes::Byte chunk_len,
                          bytes::Byte fill);

// Scripts the chunked ReadMemoryByAddress sweep from `start`, serving `data`.
void ScriptFlashReadData(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data);

// Scripts a complete zero-based ROM read serving AddressMarkedImage(length).
// The literal endpoints in the tests then catch either a shifted first
// request or a truncated final request without duplicating thousands of
// chunk expectations.
void ScriptAddressMarkedRead(ScriptedCanFlashTransport& transport, std::uint32_t length);

// Scripts the bootload handshake every operation drives: session 0x85 then
// the factory SecurityAccess seed/key pair.
void ScriptBootloadHandshake(ScriptedCanFlashTransport& transport);

// Scripts the payload half of one upload_and_commit(start, data): the
// RequestDownload and every accepted TransferData chunk, stopping before the
// checksum exchanges.
void ScriptUploadFrames(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data);

// Scripts the checksum half of one upload_and_commit: the CRC RequestDownload,
// the single CRC TransferData frame, and the RoutineControl CRC check.
// `crc_bytes`, when set, is the exact payload the test demands on the wire --
// both the value and the order of its two halves -- instead of one recomputed
// the way the implementation does.
void ScriptCrcCommit(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data,
                     std::optional<bytes::Bytes> crc_bytes = std::nullopt);

// Scripts one upload_and_commit(start, data): RequestDownload, the
// TransferData chunks, the CRC RequestDownload + TransferData, and the
// RoutineControl CRC check.
void ScriptUploadAndCommit(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data,
                           std::optional<bytes::Bytes> crc_bytes = std::nullopt);

// Scripts the unlock + erase-trigger pair.
void ScriptUnlockAndErase(ScriptedCanFlashTransport& transport);

// A complete, successful 384 KiB write of `rom` (WriteRom384()): handshake,
// page routines, erase, userspace upload and verify. No top region exists.
void ScriptWrite384KiB(ScriptedCanFlashTransport& transport, const bytes::Bytes& rom);

// A complete, successful 512 KiB write of `rom` (WriteRom()) whose top region
// differs on the ECU: the first top-region chunk mismatches, so the redirect
// bootstrap runs before the ordinary userspace write.
void ScriptWriteWithTopRegionBootstrap(ScriptedCanFlashTransport& transport, const bytes::Bytes& rom);

} // namespace fastecu::flash::colt_testing
