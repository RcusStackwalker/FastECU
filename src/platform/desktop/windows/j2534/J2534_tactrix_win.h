//////////////////////////////////////////////////////////////////////////////
//
// Copyright (C) 2004- Tactrix Inc.
//
//////////////////////////////////////////////////////////////////////////////

#pragma once

// Names follow the project style. The J2534 spec spellings are PASSTHRU_MSG ->
// PassThruMsg, SCONFIG_LIST -> SConfigList, SBYTE_ARRAY -> SByteArray. The
// exported driver symbols ("PassThruOpen", ...) are looked up by string and
// keep their spec spelling.

// "OP2.0:" refers to items that may have special relevance to the Tactrix OpenPort 2.0

#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif

/////////////////
// API Functions
/////////////////

#ifdef OP20PT32_LIB
#define PT_CALL
#define PT_API EXTERN_C
#else
#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#define PT_CALL __stdcall
#else
#define PT_CALL
#endif
#define PT_API EXTERN_C
#endif

typedef long(PT_CALL PfPassThruOpen)(const void *, unsigned long *);
typedef long(PT_CALL PfPassThruClose)(unsigned long);
typedef long(PT_CALL PfPassThruConnect)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long *);
typedef long(PT_CALL PfPassThruDisconnect)(unsigned long);
typedef long(PT_CALL PfPassThruReadMsgs)(unsigned long, void *, unsigned long *, unsigned long);
typedef long(PT_CALL PfPassThruWriteMsgs)(unsigned long, const void *, unsigned long *, unsigned long);
typedef long(PT_CALL PfPassThruStartPeriodicMsg)(unsigned long, const void *, unsigned long *, unsigned long);
typedef long(PT_CALL PfPassThruStopPeriodicMsg)(unsigned long, unsigned long);
typedef long(PT_CALL PfPassThruStartMsgFilter)(unsigned long, unsigned long, const void *, const void *, const void *,
                                               unsigned long *);
typedef long(PT_CALL PfPassThruStopMsgFilter)(unsigned long, unsigned long);
typedef long(PT_CALL PfPassThruSetProgrammingVoltage)(unsigned long, unsigned long, unsigned long);
typedef long(PT_CALL PfPassThruReadVersion)(unsigned long, char *, char *, char *);
typedef long(PT_CALL PfPassThruGetLastError)(char *);
typedef long(PT_CALL PfPassThruIoctl)(unsigned long, unsigned long, const void *, void *);

typedef void (*PfStatusCallback)(const char *, int, int);

#include "src/backend/protocol/j2534/j2534_constants.h"
#include "src/backend/protocol/j2534/tactrix_constants.h"

// Native J2534 ABI declarations. Numeric definitions are shared across platforms.

typedef struct
{
    unsigned long protocol_id;
    unsigned long rx_status;
    unsigned long tx_flags;
    unsigned long timestamp;
    unsigned long data_size;
    unsigned long extra_data_index;
    // The J2534 API defines this struct's layout; the vendor DLL reads and
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
