#pragma once

#include <cstdint>

// Numbers that cross the SerialPortActions facade: its return codes and the
// parameter ids callers pass to set_kline_timings() / set_j2534_ioctl().
// Callers include this instead of reaching the direct backend's header.
//
// STATUS_* and SERIAL_P* stay macros: the Windows SDK's ntstatus.h defines
// STATUS_SUCCESS as a macro, which would break a constexpr of that name in
// any translation unit including both.
#define STATUS_SUCCESS 0x00
#define STATUS_ERROR 0x01

#define SERIAL_P1_MIN 0x00 // J2534 says this may not be changed
#define SERIAL_P1_MAX 0x01
#define SERIAL_P2_MIN 0x02 // J2534 says this may not be changed
#define SERIAL_P2_MAX 0x03 // J2534 says this may not be changed
#define SERIAL_P3_MIN 0x04
#define SERIAL_P3_MAX 0x05 // J2534 says this may not be changed
#define SERIAL_P4_MIN 0x06
#define SERIAL_P4_MAX 0x07 // J2534 says this may not be changed

// The J2534 P1_MAX IOCTL parameter id (0x07 in both J2534_tactrix_*.h),
// named here so UI callers need no J2534 header.
inline constexpr std::uint32_t kJ2534IoctlP1Max = 0x07;
