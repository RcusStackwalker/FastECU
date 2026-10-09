#pragma once

#include "J2534_tactrix_win.h"

#include <string>
#include <windows.h>

class J2534BridgeClient
{
  public:
    J2534BridgeClient(std::string hostExePath, std::string vendorDllPath);
    ~J2534BridgeClient();

    bool start();
    bool isRunning() const
    {
        return running_;
    }

    long PassThruOpen(const void *pName, unsigned long *pDeviceID);
    long PassThruClose(unsigned long DeviceID);
    long PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags, unsigned long Baudrate,
                         unsigned long *pChannelID);
    long PassThruDisconnect(unsigned long ChannelID);
    long PassThruReadMsgs(unsigned long ChannelID, PassThruMsg *pMsg, unsigned long *pNumMsgs, unsigned long Timeout);
    long PassThruWriteMsgs(unsigned long ChannelID, const PassThruMsg *pMsg, unsigned long *pNumMsgs,
                           unsigned long Timeout);
    long PassThruStartPeriodicMsg(unsigned long ChannelID, const PassThruMsg *pMsg, unsigned long *pMsgID,
                                  unsigned long TimeInterval);
    long PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID);
    long PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType, const PassThruMsg *pMaskMsg,
                                const PassThruMsg *pPatternMsg, const PassThruMsg *pFlowControlMsg,
                                unsigned long *pMsgID);
    long PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID);
    long PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin, unsigned long Voltage);
    long PassThruReadVersion(char *pApiVersion, char *pDllVersion, char *pFirmwareVersion, unsigned long DeviceID);
    long PassThruGetLastError(char *pErrorDescription);
    long PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput);

  private:
    std::string host_exe_path_;
    std::string vendor_dll_path_;
    bool running_ = false;

    HANDLE to_child_write_ = nullptr;
    HANDLE from_child_read_ = nullptr;
    HANDLE job_object_ = nullptr;
    PROCESS_INFORMATION process_info_{};

    void stop();
};
