#pragma once

#include "src/backend/protocol/j2534/j2534_constants.h"
#include "src/backend/protocol/j2534/tactrix_constants.h"

// Native J2534 ABI declarations. Numeric definitions are shared across platforms.

typedef struct
{
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;
    unsigned long DataSize;
    unsigned long ExtraDataIndex;
    // The J2534 API defines this struct's layout; the vendor library reads and
    // writes it in place, so the trailing payload stays a C array.
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    unsigned char Data[kJ2534PassthruMsgDataSize];
} PASSTHRU_MSG;

////////////////
// IOCTL structs
////////////////

typedef struct
{
    unsigned long Parameter;
    unsigned long Value;
} SCONFIG;

typedef struct
{
    unsigned long NumOfParams;
    SCONFIG *ConfigPtr;
} SCONFIG_LIST;

typedef struct
{
    unsigned long NumOfBytes;
    unsigned char *BytePtr;
} SBYTE_ARRAY;
