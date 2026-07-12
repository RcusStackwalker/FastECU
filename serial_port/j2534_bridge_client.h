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
    long PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags,
                         unsigned long Baudrate, unsigned long *pChannelID);
    long PassThruDisconnect(unsigned long ChannelID);
    long PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs, unsigned long Timeout);
    long PassThruWriteMsgs(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs,
                           unsigned long Timeout);
    long PassThruStartPeriodicMsg(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pMsgID,
                                  unsigned long TimeInterval);
    long PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID);
    long PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType, const PASSTHRU_MSG *pMaskMsg,
                                const PASSTHRU_MSG *pPatternMsg, const PASSTHRU_MSG *pFlowControlMsg,
                                unsigned long *pMsgID);
    long PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID);
    long PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin, unsigned long Voltage);
    long PassThruReadVersion(char *pApiVersion, char *pDllVersion, char *pFirmwareVersion, unsigned long DeviceID);
    long PassThruGetLastError(char *pErrorDescription);
    long PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput);

  private:
    std::string hostExePath_;
    std::string vendorDllPath_;
    bool running_ = false;

    HANDLE toChildWrite_ = nullptr;
    HANDLE fromChildRead_ = nullptr;
    HANDLE jobObject_ = nullptr;
    PROCESS_INFORMATION processInfo_{};

    void stop();
};
