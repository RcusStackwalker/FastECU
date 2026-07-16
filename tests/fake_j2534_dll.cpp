#include "J2534_tactrix_win.h"

#include <cstring>

extern "C"
{

    __declspec(dllexport) long PT_CALL PassThruOpen(const void * /*pName*/, unsigned long *pDeviceID)
    {
        *pDeviceID = 7;
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruClose(unsigned long /*DeviceID*/)
    {
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruConnect(unsigned long /*DeviceID*/, unsigned long /*ProtocolID*/,
                                                       unsigned long /*Flags*/, unsigned long /*Baudrate*/,
                                                       unsigned long *pChannelID)
    {
        *pChannelID = 3;
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruDisconnect(unsigned long /*ChannelID*/)
    {
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruReadMsgs(unsigned long /*ChannelID*/, PASSTHRU_MSG *pMsg,
                                                        unsigned long *pNumMsgs, unsigned long /*Timeout*/)
    {
        pMsg[0].DataSize = 4;
        pMsg[0].Data[0] = 0xDE;
        pMsg[0].Data[1] = 0xAD;
        pMsg[0].Data[2] = 0xBE;
        pMsg[0].Data[3] = 0xEF;
        *pNumMsgs = 1;
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruWriteMsgs(unsigned long /*ChannelID*/, const PASSTHRU_MSG *pMsg,
                                                         unsigned long *pNumMsgs, unsigned long /*Timeout*/)
    {
        *pNumMsgs = 1;
        return (pMsg[0].DataSize > 0 && pMsg[0].Data[0] == 0x11) ? STATUS_NOERROR : ERR_FAILED;
    }

    __declspec(dllexport) long PT_CALL PassThruStartPeriodicMsg(unsigned long /*ChannelID*/, const PASSTHRU_MSG * /*pMsg*/,
                                                                unsigned long *pMsgID, unsigned long /*TimeInterval*/)
    {
        *pMsgID = 55;
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruStopPeriodicMsg(unsigned long /*ChannelID*/, unsigned long /*MsgID*/)
    {
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruStartMsgFilter(unsigned long /*ChannelID*/, unsigned long /*FilterType*/,
                                                              const void * /*pMaskMsg*/, const void * /*pPatternMsg*/,
                                                              const void * /*pFlowControlMsg*/, unsigned long *pMsgID)
    {
        *pMsgID = 99;
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruStopMsgFilter(unsigned long /*ChannelID*/, unsigned long /*MsgID*/)
    {
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruSetProgrammingVoltage(unsigned long /*DeviceID*/, unsigned long /*Pin*/,
                                                                     unsigned long /*Voltage*/)
    {
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruReadVersion(unsigned long /*DeviceID*/, char *pApiVersion,
                                                           char *pDllVersion, char *pFirmwareVersion)
    {
        std::strcpy(pApiVersion, "04.04");
        std::strcpy(pDllVersion, "1.0.0-fake");
        std::strcpy(pFirmwareVersion, "0.0.0-fake");
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruGetLastError(char *pErrorDescription)
    {
        std::strcpy(pErrorDescription, "fake DLL error");
        return STATUS_NOERROR;
    }

    __declspec(dllexport) long PT_CALL PassThruIoctl(unsigned long /*ChannelID*/, unsigned long IoctlID,
                                                     const void * /*pInput*/, void *pOutput)
    {
        if (IoctlID == READ_VBATT)
        {
            *static_cast<unsigned long *>(pOutput) = 12500;
        }
        return STATUS_NOERROR;
    }

} // extern "C"
