#pragma once

#include "src/backend/protocol/j2534/j2534_constants.h"
#include "src/backend/protocol/j2534/tactrix_constants.h"

// Native J2534 ABI declarations. Numeric definitions are shared across platforms.
// Names follow the project style. The J2534 spec spellings are PASSTHRU_MSG ->
// PassThruMsg, SCONFIG_LIST -> SConfigList, SBYTE_ARRAY -> SByteArray. The
// exported driver symbols ("PassThruOpen", ...) are looked up by string and
// keep their spec spelling.

typedef struct
{
    unsigned long protocol_id;
    unsigned long rx_status;
    unsigned long tx_flags;
    unsigned long timestamp;
    unsigned long data_size;
    unsigned long extra_data_index;
    // The J2534 API defines this struct's layout; the vendor library reads and
    // writes it in place, so the trailing payload stays a C array.
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    unsigned char data[kJ2534PassthruMsgDataSize];
} PassThruMsg;

////////////////
// IOCTL structs
////////////////

typedef struct
{
    unsigned long parameter;
    unsigned long value;
} SCONFIG;

typedef struct
{
    unsigned long num_of_params;
    SCONFIG *config_ptr;
} SConfigList;

typedef struct
{
    unsigned long num_of_bytes;
    unsigned char *byte_ptr;
} SByteArray;
