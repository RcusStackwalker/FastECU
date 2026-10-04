#pragma once

// Numbers that cross the SerialPortActions facade: its return codes and the
// parameter ids callers pass to set_kline_timings().
// Callers include this instead of reaching the direct backend's header.
// Status names deliberately avoid the Windows SDK STATUS_SUCCESS macro.
inline constexpr int kSerialSuccess = 0x00;
inline constexpr int kSerialError = 0x01;

inline constexpr int kSerialP1Min = 0x00; // J2534 says this may not be changed
inline constexpr int kSerialP1Max = 0x01;
inline constexpr int kSerialP2Min = 0x02; // J2534 says this may not be changed
inline constexpr int kSerialP2Max = 0x03; // J2534 says this may not be changed
inline constexpr int kSerialP3Min = 0x04;
inline constexpr int kSerialP3Max = 0x05; // J2534 says this may not be changed
inline constexpr int kSerialP4Min = 0x06;
inline constexpr int kSerialP4Max = 0x07; // J2534 says this may not be changed
