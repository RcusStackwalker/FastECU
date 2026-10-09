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

typedef long(PT_CALL PF_PassThruOpen)(const void *, unsigned long *);
typedef long(PT_CALL PF_PassThruClose)(unsigned long);
typedef long(PT_CALL PF_PassThruConnect)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long *);
typedef long(PT_CALL PF_PassThruDisconnect)(unsigned long);
typedef long(PT_CALL PF_PassThruReadMsgs)(unsigned long, void *, unsigned long *, unsigned long);
typedef long(PT_CALL PF_PassThruWriteMsgs)(unsigned long, const void *, unsigned long *, unsigned long);
typedef long(PT_CALL PF_PassThruStartPeriodicMsg)(unsigned long, const void *, unsigned long *, unsigned long);
typedef long(PT_CALL PF_PassThruStopPeriodicMsg)(unsigned long, unsigned long);
typedef long(PT_CALL PF_PassThruStartMsgFilter)(unsigned long, unsigned long, const void *, const void *, const void *,
                                                unsigned long *);
typedef long(PT_CALL PF_PassThruStopMsgFilter)(unsigned long, unsigned long);
typedef long(PT_CALL PF_PassThruSetProgrammingVoltage)(unsigned long, unsigned long, unsigned long);
typedef long(PT_CALL PF_PassThruReadVersion)(unsigned long, char *, char *, char *);
typedef long(PT_CALL PF_PassThruGetLastError)(char *);
typedef long(PT_CALL PF_PassThruIoctl)(unsigned long, unsigned long, const void *, void *);

typedef void (*PF_StatusCallback)(const char *, int, int);

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
    // The J2534 API defines this struct's layout; the vendor DLL reads and
    // writes it in place, so the trailing payload stays a C array.
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    unsigned char Data[kJ2534PassthruMsgDataSize];
} PassThruMsg;

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
} SConfigList;

typedef struct
{
    unsigned long NumOfBytes;
    unsigned char *BytePtr;
} SByteArray;
