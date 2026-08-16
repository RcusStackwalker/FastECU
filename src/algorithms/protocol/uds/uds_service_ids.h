#pragma once

#include "src/algorithms/protocol/bytes.h"

// Named bytes for the request/response vocabulary shared, byte-for-byte,
// across this codebase's Subaru CAN and K-Line bootloader/kernel executors
// (subaru_hitachi_m32r_can, subaru_tcu_cvt_hitachi_m32r_can,
// subaru_tcu_cvt_mitsu_mh8104_can, subaru_tcu_cvt_mitsu_mh8111_can,
// subaru_hitachi_m32r_kline, subaru_mitsu_m32r_kline). Each group below is
// labeled with the standard it comes from, or "vendor/kernel-specific" when
// it isn't standardized at all -- these bootloader kernels mix real ISO
// 14229 (UDS) and SAE J1979 (OBD-II) services with services of their own.
// Values that vary per family (session ids in the ISO 14229 manufacturer-
// specific band, routineIdentifiers, K-Line's own handshake bytes) are NOT
// here; they stay as named constants local to each executor file.
namespace uds
{

// Service identifiers actually observed on these buses.
constexpr bytes::Byte kSidDiagnosticSessionControl = 0x10; // ISO 14229-1
constexpr bytes::Byte kSidSecurityAccess = 0x27;           // ISO 14229-1
constexpr bytes::Byte kSidRoutineControl = 0x31;           // ISO 14229-1
constexpr bytes::Byte kSidRequestDownload = 0x34;          // ISO 14229-1
constexpr bytes::Byte kSidRequestUpload = 0x35;            // ISO 14229-1
constexpr bytes::Byte kSidTransferData = 0x36;             // ISO 14229-1
constexpr bytes::Byte kSidRequestTransferExit = 0x37;      // ISO 14229-1
constexpr bytes::Byte kSidVehicleInfoRequest = 0x09;       // SAE J1979 Mode $09
constexpr bytes::Byte kSidEcuIdQuery = 0xAA;               // vendor/kernel-specific
constexpr bytes::Byte kSidReadMemoryChunk = 0xB7;          // vendor/kernel-specific
constexpr bytes::Byte kSidWriteMemoryChunk = 0xB6;         // vendor/kernel-specific

// DiagnosticSessionControl subfunctions that are standard ISO 14229-1
// values (0x01 defaultSession and 0x40-0x5F vehicleManufacturerSpecific are
// not: the former is unused on this bus, the latter is family-specific and
// named per file).
constexpr bytes::Byte kSessionProgramming = 0x02;
constexpr bytes::Byte kSessionExtendedDiagnostic = 0x03;

// SecurityAccess subfunctions: ISO 14229-1 pairs odd = requestSeed with the
// next even value = sendKey for the same level. Every family here uses
// level 1 (0x01/0x02).
constexpr bytes::Byte kSecurityAccessRequestSeed = 0x01;
constexpr bytes::Byte kSecurityAccessSendKey = 0x02;

// RoutineControl's leading subfunction byte, ISO 14229-1 standard verbs.
// The routineIdentifier that follows is vendor-assigned per ISO 14229-1 and
// stays local to each family -- it is not part of the standard.
constexpr bytes::Byte kRoutineControlStart = 0x01;
constexpr bytes::Byte kRoutineControlStop = 0x02;
constexpr bytes::Byte kRoutineControlRequestResults = 0x03;

// SAE J1979 Mode $09 ("Request Vehicle Information") PIDs.
constexpr bytes::Byte kVehicleInfoPidVin = 0x02;
constexpr bytes::Byte kVehicleInfoPidCalId = 0x04;
constexpr bytes::Byte kVehicleInfoPidCvn = 0x06;

} // namespace uds
