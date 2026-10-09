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
#define PTfn(name) Pf##name *pf##name
#define PText(name) PT_API Pf##name name

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

    bool is_serial_port_open();
    bool init()
    {
        return checkDLL();
    };
    void disable();
    void setDllName(const char *name);
    void getDllName(char *name);
    bool valid();
    char *getLastError();

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
    bool getPTfns();
    long LoadJ2534DLL(const char *sz_dll);
    bool checkDLL();
    int is_valid_sconfig_param(SCONFIG s);

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

    PTfn(PassThruOpen);
    PTfn(PassThruClose);
    PTfn(PassThruConnect);
    PTfn(PassThruDisconnect);
    PTfn(PassThruReadMsgs);
    PTfn(PassThruWriteMsgs);
    PTfn(PassThruStartPeriodicMsg);
    PTfn(PassThruStopPeriodicMsg);
    PTfn(PassThruStartMsgFilter);
    PTfn(PassThruStopMsgFilter);
    PTfn(PassThruSetProgrammingVoltage);
    PTfn(PassThruReadVersion);
    PTfn(PassThruGetLastError);
    PTfn(PassThruIoctl);
};

#if defined(OP20PT32_USE_LIB)
//	PT_API void OP20PT32_Start();
//	PT_API void OP20PT32_Stop();
PText(PassThruOpen);
PText(PassThruClose);
PText(PassThruConnect);
PText(PassThruDisconnect);
PText(PassThruReadMsgs);
PText(PassThruWriteMsgs);
PText(PassThruStartPeriodicMsg);
PText(PassThruStopPeriodicMsg);
PText(PassThruStartMsgFilter);
PText(PassThruStopMsgFilter);
PText(PassThruSetProgrammingVoltage);
PText(PassThruReadVersion);
PText(PassThruGetLastError);
PText(PassThruIoctl);
#endif
