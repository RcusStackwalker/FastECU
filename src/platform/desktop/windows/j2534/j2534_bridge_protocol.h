#pragma once

#include "J2534_tactrix_win.h"

#include <array>
#include <cstdint>
#include <windows.h>

namespace j2534_bridge
{

enum class Function : std::uint8_t
{
    // Never sent. The value of a zero-initialised FrameHeader before
    // readFrameHeader() fills it in, and what a corrupt frame decodes to.
    kInvalid = 0,
    kPassThruOpen = 1,
    kPassThruClose = 2,
    kPassThruConnect = 3,
    kPassThruDisconnect = 4,
    kPassThruReadMsgs = 5,
    kPassThruWriteMsgs = 6,
    kPassThruStartPeriodicMsg = 7,
    kPassThruStopPeriodicMsg = 8,
    kPassThruStartMsgFilter = 9,
    kPassThruStopMsgFilter = 10,
    kPassThruSetProgrammingVoltage = 11,
    kPassThruReadVersion = 12,
    kPassThruGetLastError = 13,
    kPassThruIoctl = 14,
    kShutdown = 255,
};

struct FrameHeader
{
    Function function = Function::kInvalid;
    std::uint32_t payload_size = 0;
};

struct PassThruOpenRequest
{
    bool has_name;
    std::array<char, 256> name;
};
struct PassThruOpenResponse
{
    long result;
    unsigned long device_id;
};

struct PassThruCloseRequest
{
    unsigned long device_id;
};
struct PassThruCloseResponse
{
    long result;
};

struct PassThruConnectRequest
{
    unsigned long device_id;
    unsigned long protocol_id;
    unsigned long flags;
    unsigned long baudrate;
};
struct PassThruConnectResponse
{
    long result;
    unsigned long channel_id;
};

struct PassThruDisconnectRequest
{
    unsigned long channel_id;
};
struct PassThruDisconnectResponse
{
    long result;
};

// This codebase always passes NumMsgs == 1 (see serial_port_actions_direct.cpp) --
// the bridge is deliberately single-message, not a general array marshaler.
struct PassThruReadMsgsRequest
{
    unsigned long channel_id;
    unsigned long timeout;
};
struct PassThruReadMsgsResponse
{
    long result;
    unsigned long num_msgs; // 0 or 1
    PassThruMsg msg;
};

struct PassThruWriteMsgsRequest
{
    unsigned long channel_id;
    unsigned long timeout;
    PassThruMsg msg;
};
struct PassThruWriteMsgsResponse
{
    long result;
    unsigned long num_msgs; // 0 or 1
};

struct PassThruStartPeriodicMsgRequest
{
    unsigned long channel_id;
    unsigned long time_interval;
    PassThruMsg msg;
};
struct PassThruStartPeriodicMsgResponse
{
    long result;
    unsigned long msg_id;
};

struct PassThruStopPeriodicMsgRequest
{
    unsigned long channel_id;
    unsigned long msg_id;
};
struct PassThruStopPeriodicMsgResponse
{
    long result;
};

struct PassThruStartMsgFilterRequest
{
    unsigned long channel_id;
    unsigned long filter_type;
    bool has_flow_control_msg; // false for PASS_FILTER/BLOCK_FILTER (pFlowControlMsg == NULL)
    PassThruMsg mask_msg;
    PassThruMsg pattern_msg;
    PassThruMsg flow_control_msg; // meaningful only when has_flow_control_msg is true
};
struct PassThruStartMsgFilterResponse
{
    long result;
    unsigned long msg_id;
};

struct PassThruStopMsgFilterRequest
{
    unsigned long channel_id;
    unsigned long msg_id;
};
struct PassThruStopMsgFilterResponse
{
    long result;
};

struct PassThruSetProgrammingVoltageRequest
{
    unsigned long device_id;
    unsigned long pin;
    unsigned long voltage;
};
struct PassThruSetProgrammingVoltageResponse
{
    long result;
};

struct PassThruReadVersionRequest
{
    unsigned long device_id;
};
struct PassThruReadVersionResponse
{
    long result;
    std::array<char, 80> api_version;
    std::array<char, 80> dll_version;
    std::array<char, 80> firmware_version;
};

struct PassThruGetLastErrorRequest
{
    std::uint8_t unused; // no real fields; kept non-empty for a well-defined sizeof
};
struct PassThruGetLastErrorResponse
{
    long result;
    std::array<char, 80> error_description;
};

// PassThruIoctl: only the closed set of IoctlIDs this codebase actually issues
// (see J2534_tactrix_win.h's IOCTL ID table and serial_port_actions_direct.cpp's
// call sites) gets a typed shape. Any other IoctlID is rejected by the host
// with ERR_INVALID_IOCTL_ID before it ever reaches the vendor DLL.
struct PassThruIoctlRequest
{
    unsigned long channel_id;
    unsigned long ioctl_id;
    unsigned long num_config_params;           // SET_CONFIG only
    std::array<SCONFIG, 16> config_params;     // SET_CONFIG only; this codebase never sets more than a handful
    unsigned long input_byte_count;            // FIVE_BAUD_INIT / FAST_INIT only
    std::array<unsigned char, 64> input_bytes; // FIVE_BAUD_INIT / FAST_INIT only
};
struct PassThruIoctlResponse
{
    long result;
    unsigned long output_byte_count;            // FIVE_BAUD_INIT / FAST_INIT only
    std::array<unsigned char, 64> output_bytes; // FIVE_BAUD_INIT / FAST_INIT only
    unsigned long vbatt;                        // READ_VBATT / READ_PROG_VOLTAGE only
};

bool writeFrame(HANDLE pipe, Function function, const void *payload, std::uint32_t payloadSize);

// Split out of readFrame so callers that don't yet know which *Request struct
// to decode (e.g. the bridge host's dispatch loop) can read the fixed-size
// header first and only then read the payload once the Function tag is known.
bool readFrameHeader(HANDLE pipe, FrameHeader& outHeader);
bool readFramePayload(HANDLE pipe, void *payload, std::uint32_t payloadSize);

// Convenience wrapper for callers that already know their expected payload
// type up front (e.g. the protocol test): reads the header, then the payload.
bool readFrame(HANDLE pipe, FrameHeader& outHeader, void *payload, std::uint32_t payloadCapacity);

} // namespace j2534_bridge
