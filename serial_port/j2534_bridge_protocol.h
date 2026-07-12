#pragma once

#include "J2534_tactrix_win.h"

#include <cstdint>
#include <windows.h>

namespace j2534_bridge
{

enum class Function : std::uint8_t
{
    PassThruOpen = 1,
    PassThruClose = 2,
    PassThruConnect = 3,
    PassThruDisconnect = 4,
    PassThruReadMsgs = 5,
    PassThruWriteMsgs = 6,
    PassThruStartPeriodicMsg = 7,
    PassThruStopPeriodicMsg = 8,
    PassThruStartMsgFilter = 9,
    PassThruStopMsgFilter = 10,
    PassThruSetProgrammingVoltage = 11,
    PassThruReadVersion = 12,
    PassThruGetLastError = 13,
    PassThruIoctl = 14,
    Shutdown = 255,
};

struct FrameHeader
{
    Function function;
    std::uint32_t payloadSize;
};

struct PassThruOpenRequest
{
    bool hasName;
    char name[256];
};
struct PassThruOpenResponse
{
    long result;
    unsigned long deviceId;
};

struct PassThruCloseRequest
{
    unsigned long deviceId;
};
struct PassThruCloseResponse
{
    long result;
};

struct PassThruConnectRequest
{
    unsigned long deviceId;
    unsigned long protocolId;
    unsigned long flags;
    unsigned long baudrate;
};
struct PassThruConnectResponse
{
    long result;
    unsigned long channelId;
};

struct PassThruDisconnectRequest
{
    unsigned long channelId;
};
struct PassThruDisconnectResponse
{
    long result;
};

// This codebase always passes NumMsgs == 1 (see serial_port_actions_direct.cpp) --
// the bridge is deliberately single-message, not a general array marshaler.
struct PassThruReadMsgsRequest
{
    unsigned long channelId;
    unsigned long timeout;
};
struct PassThruReadMsgsResponse
{
    long result;
    unsigned long numMsgs; // 0 or 1
    PASSTHRU_MSG msg;
};

struct PassThruWriteMsgsRequest
{
    unsigned long channelId;
    unsigned long timeout;
    PASSTHRU_MSG msg;
};
struct PassThruWriteMsgsResponse
{
    long result;
    unsigned long numMsgs; // 0 or 1
};

struct PassThruStartPeriodicMsgRequest
{
    unsigned long channelId;
    unsigned long timeInterval;
    PASSTHRU_MSG msg;
};
struct PassThruStartPeriodicMsgResponse
{
    long result;
    unsigned long msgId;
};

struct PassThruStopPeriodicMsgRequest
{
    unsigned long channelId;
    unsigned long msgId;
};
struct PassThruStopPeriodicMsgResponse
{
    long result;
};

struct PassThruStartMsgFilterRequest
{
    unsigned long channelId;
    unsigned long filterType;
    bool hasFlowControlMsg; // false for PASS_FILTER/BLOCK_FILTER (pFlowControlMsg == NULL)
    PASSTHRU_MSG maskMsg;
    PASSTHRU_MSG patternMsg;
    PASSTHRU_MSG flowControlMsg; // meaningful only when hasFlowControlMsg is true
};
struct PassThruStartMsgFilterResponse
{
    long result;
    unsigned long msgId;
};

struct PassThruStopMsgFilterRequest
{
    unsigned long channelId;
    unsigned long msgId;
};
struct PassThruStopMsgFilterResponse
{
    long result;
};

struct PassThruSetProgrammingVoltageRequest
{
    unsigned long deviceId;
    unsigned long pin;
    unsigned long voltage;
};
struct PassThruSetProgrammingVoltageResponse
{
    long result;
};

struct PassThruReadVersionRequest
{
    unsigned long deviceId;
};
struct PassThruReadVersionResponse
{
    long result;
    char apiVersion[80];
    char dllVersion[80];
    char firmwareVersion[80];
};

struct PassThruGetLastErrorRequest
{
    std::uint8_t unused; // no real fields; kept non-empty for a well-defined sizeof
};
struct PassThruGetLastErrorResponse
{
    long result;
    char errorDescription[80];
};

// PassThruIoctl: only the closed set of IoctlIDs this codebase actually issues
// (see J2534_tactrix_win.h's IOCTL ID table and serial_port_actions_direct.cpp's
// call sites) gets a typed shape. Any other IoctlID is rejected by the host
// with ERR_INVALID_IOCTL_ID before it ever reaches the vendor DLL.
struct PassThruIoctlRequest
{
    unsigned long channelId;
    unsigned long ioctlId;
    unsigned long numConfigParams; // SET_CONFIG only
    SCONFIG configParams[16];      // SET_CONFIG only; this codebase never sets more than a handful
    unsigned long inputByteCount;  // FIVE_BAUD_INIT / FAST_INIT only
    unsigned char inputBytes[64];  // FIVE_BAUD_INIT / FAST_INIT only
};
struct PassThruIoctlResponse
{
    long result;
    unsigned long outputByteCount; // FIVE_BAUD_INIT / FAST_INIT only
    unsigned char outputBytes[64]; // FIVE_BAUD_INIT / FAST_INIT only
    unsigned long vbatt;           // READ_VBATT / READ_PROG_VOLTAGE only
};

bool writeFrame(HANDLE pipe, Function function, const void *payload, std::uint32_t payloadSize);
bool readFrame(HANDLE pipe, FrameHeader& outHeader, void *payload, std::uint32_t payloadCapacity);

} // namespace j2534_bridge
