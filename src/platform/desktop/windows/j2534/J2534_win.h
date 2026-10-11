//////////////////////////////////////////////////////////////////////////////
//
// Copyright (C) 2004- Tactrix Inc.
//
// You are free to use this file for any purpose, but please keep
// notice of where it came from!
//
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <array>
#include <cstddef>
#include <memory>

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#include <windows.h>
#endif

#include "J2534_tactrix_win.h"
#include "src/platform/desktop/windows/j2534/j2534_bridge_client.h"

// name is token-pasted into a type and a member name, so it cannot be parenthesized.
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define J2534_PT_FN(name) Pf##name *pf##name
#define J2534_PT_EXT(name) PT_API Pf##name name

class J2534
{
  public:
    J2534();
    ~J2534();

    bool serial_port_protocol_iso14230 = false;
    bool j2534_init_ok = false;

    // Matches J2534_unix.h's contract: PassThruReadVersion's out-parameters
    // must each point at a buffer of at least this many bytes.
    static constexpr std::size_t kVersionBufferSize = 256;

    bool IsSerialPortOpen();
    bool Init()
    {
        return CheckDll();
    };
    void Disable();
    void SetDllName(const char *name);
    void GetDllName(char *name);
    bool Valid();
    char *GetLastError();

    long PassThruOpen(const void *p_name, unsigned long *p_device_id);
    long PassThruClose(unsigned long device_id);
    long PassThruConnect(unsigned long device_id, unsigned long protocol_id, unsigned long flags,
                         unsigned long baudrate, unsigned long *p_channel_id);
    long PassThruDisconnect(unsigned long channel_id);
    long PassThruReadMsgs(unsigned long channel_id, PassThruMsg *p_msg, unsigned long *p_num_msgs,
                          unsigned long timeout);
    long PassThruWriteMsgs(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_num_msgs,
                           unsigned long timeout);
    long PassThruStartPeriodicMsg(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_msg_id,
                                  unsigned long time_interval);
    long PassThruStopPeriodicMsg(unsigned long channel_id, unsigned long msg_id);
    long PassThruStartMsgFilter(unsigned long channel_id, unsigned long filter_type, const PassThruMsg *p_mask_msg,
                                const PassThruMsg *p_pattern_msg, const PassThruMsg *p_flow_control_msg,
                                unsigned long *p_msg_id);
    long PassThruStopMsgFilter(unsigned long channel_id, unsigned long msg_id);
    long PassThruSetProgrammingVoltage(unsigned long device_id, unsigned long pin, unsigned long voltage);
    long PassThruReadVersion(char *p_api_version, char *p_dll_version, char *p_firmware_version,
                             unsigned long device_id);
    long PassThruGetLastError(char *p_error_description);
    long PassThruIoctl(unsigned long channel_id, unsigned long ioctl_id, const void *p_input, void *p_output);

  private:
    bool GetPTfns();
    long LoadJ2534DLL(const char *sz_dll);
    bool CheckDll();
    int IsValidSconfigParam(SCONFIG s);

    std::array<char, 256> last_error_;
    std::array<char, 256> dll_name_;
    bool is_library_initialized_;

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    HINSTANCE h_dll_; // Handle to DLL
#else
    void *h_dll_;
#endif

    std::unique_ptr<J2534BridgeClient> bridge_client_;
    bool use_bridge_ = false;

    /* J2534 Interface API function pointers */

    J2534_PT_FN(PassThruOpen);
    J2534_PT_FN(PassThruClose);
    J2534_PT_FN(PassThruConnect);
    J2534_PT_FN(PassThruDisconnect);
    J2534_PT_FN(PassThruReadMsgs);
    J2534_PT_FN(PassThruWriteMsgs);
    J2534_PT_FN(PassThruStartPeriodicMsg);
    J2534_PT_FN(PassThruStopPeriodicMsg);
    J2534_PT_FN(PassThruStartMsgFilter);
    J2534_PT_FN(PassThruStopMsgFilter);
    J2534_PT_FN(PassThruSetProgrammingVoltage);
    J2534_PT_FN(PassThruReadVersion);
    J2534_PT_FN(PassThruGetLastError);
    J2534_PT_FN(PassThruIoctl);
};

#if defined(OP20PT32_USE_LIB)
//	PT_API void OP20PT32_Start();
//	PT_API void OP20PT32_Stop();
J2534_PT_EXT(PassThruOpen);
J2534_PT_EXT(PassThruClose);
J2534_PT_EXT(PassThruConnect);
J2534_PT_EXT(PassThruDisconnect);
J2534_PT_EXT(PassThruReadMsgs);
J2534_PT_EXT(PassThruWriteMsgs);
J2534_PT_EXT(PassThruStartPeriodicMsg);
J2534_PT_EXT(PassThruStopPeriodicMsg);
J2534_PT_EXT(PassThruStartMsgFilter);
J2534_PT_EXT(PassThruStopMsgFilter);
J2534_PT_EXT(PassThruSetProgrammingVoltage);
J2534_PT_EXT(PassThruReadVersion);
J2534_PT_EXT(PassThruGetLastError);
J2534_PT_EXT(PassThruIoctl);
#endif
