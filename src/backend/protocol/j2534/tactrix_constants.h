//////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2004- Tactrix Inc.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include "src/backend/protocol/j2534/j2534_constants.h"

// OpenPort additions and aliases from FastECU's existing Tactrix headers.
// These values depend on the vendor API, not on the host operating system.
inline constexpr int kJ2534Iso9141K = kJ2534Iso9141Ch1;
inline constexpr int kJ2534Iso9141L = kJ2534Iso9141Ch2; // OP2.0: Support for ISO9141 communications over the L line
inline constexpr int kJ2534Iso9141Inno =
    kJ2534Iso9141Ch3; // OP2.0: Support for RS-232 receive-only communications via the 2.5mm jack
inline constexpr int kJ2534Iso14230K = kJ2534Iso14230Ch1;
inline constexpr int kJ2534Iso14230L = kJ2534Iso14230Ch2; // OP2.0: Support for ISO14230 communications over the L line
inline constexpr int kJ2534DstiIso9141 = 0x00020001;
inline constexpr int kJ2534PinVadj = 17; // internal	OP2.0: Supports reading voltage
inline constexpr int kJ2534TxParamBase = 0x9000;
inline constexpr int kJ2534ErrOemVoltageTooLow =
    0x78; // OP2.0: the requested output voltage is lower than the OP2.0 capabilities
inline constexpr int kJ2534ErrOemVoltageTooHigh =
    0x77; // OP2.0: the requested output voltage is higher than the OP2.0 capabilities
inline constexpr int kJ2534SniffMode = 0x10000000; // OP2.0: listens to a bus (e.g. CAN) without acknowledging

// Legacy Tactrix header values, retained for compatibility. Current J2534-2
// references assign HS=0x8000 and NS=0x8001; the old headers reverse those names.
// Neither command is used by FastECU. Correcting them is a separate behavior fix.
// https://www.quantexlab.com/en/develop/j2534/pt_ioctl.html
inline constexpr int kJ2534SwCanNs = 0x8000; // OP2.0: Not supported
inline constexpr int kJ2534SwCanHs = 0x8001; // OP2.0: Not supported
