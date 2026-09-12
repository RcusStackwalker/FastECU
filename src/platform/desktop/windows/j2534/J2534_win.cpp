#include <array>
#include <cstdio>
#include <stdarg.h>
#include <string.h>
#include "src/platform/desktop/windows/j2534/J2534_win.h"
#include "src/platform/desktop/windows/j2534/pe_bitness.h"
#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#else
#include <dlfcn.h>
#include <CoreFoundation/CFBundle.h>
#include <unistd.h>
#endif

J2534::J2534()
{
    hDLL = nullptr;
    debugMode = false;
    isLibraryInitialized = false;
    // default to the Openport 2.0 J2534 DLL
#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    strcpy(dllName.data(), "j2534.dll");
#else
    strcpy(dllName.data(), "op20pt32.dylib");
#endif
}

void J2534::setDllName(const char *name)
{
    strcpy(dllName.data(), name);
}

void J2534::getDllName(char *name)
{
    strcpy(name, dllName.data());
}

void J2534::disable()
{
    if (hDLL)
    {
        FreeLibrary(hDLL);
        hDLL = nullptr;
    }
    if (bridgeClient)
    {
        bridgeClient.reset();
        useBridge = false;
    }
}

char *J2534::getLastError()
{
    return lastError.data();
}

bool J2534::valid()
{
    if (bridgeClient)
        return bridgeClient->isRunning();
    return hDLL != nullptr;
}

J2534::~J2534()
{
#if defined(OP20PT32_USE_LIB)
    //	if (hDLL)
    //		::OP20PT32_Stop();
    // #else

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    if (hDLL)
        FreeLibrary(hDLL);
#else
    if (hDLL)
        dlclose(hDLL);
#endif
#endif
}

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#if defined(OP20PT32_USE_LIB)
#define getPTfn(name) pf##name = ::name;
#else
#define getPTfn(name)                                                                                                  \
    if (!(pf##name = (PF_##name *)GetProcAddress(hDLL, "" #name)))                                                     \
    return false
#endif
#else
#if defined(OP20PT32_USE_LIB)
#define getPTfn(name) pf##name = ::name;
#else
#define getPTfn(name)                                                                                                  \
    if (!(pf##name = (PF_##name *)dlsym(hDLL, "" #name)))                                                              \
    return false
#endif
#endif

#define DBGPRINT_BUFSIZE 10240

bool J2534::getPTfns()
{
    if (!hDLL)
        return false;

    getPTfn(PassThruOpen);
    getPTfn(PassThruClose);
    getPTfn(PassThruConnect);
    getPTfn(PassThruDisconnect);
    getPTfn(PassThruReadMsgs);
    getPTfn(PassThruWriteMsgs);
    getPTfn(PassThruStartPeriodicMsg);
    getPTfn(PassThruStopPeriodicMsg);
    getPTfn(PassThruStartMsgFilter);
    getPTfn(PassThruStopMsgFilter);
    getPTfn(PassThruSetProgrammingVoltage);
    getPTfn(PassThruReadVersion);
    getPTfn(PassThruGetLastError);
    getPTfn(PassThruIoctl);

    return true;
}

void J2534::dbgprint(const char *Format, ...)
{
    va_list arglist;
    int cb;
    std::array<char, DBGPRINT_BUFSIZE> buffer;
    char *pbuf = buffer.data();

    va_start(arglist, Format);

    cb = vsprintf(pbuf, Format, arglist);

    if (cb == -1)
    {
        buffer[buffer.size() - 2] = '\n';
        buffer[buffer.size() - 1] = '\0';
    }

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    OutputDebugStringA(buffer.data());
#else
    printf("%s", buffer.data());
#endif
    va_end(arglist);
}

#define MSG_READ 1
#define MSG_WRITE 2

void J2534::dbgdump(const unsigned char *data, unsigned int datalen, int kind)
{
    unsigned int i;
    std::array<char, DBGPRINT_BUFSIZE> buf;
    char *pbuf = buf.data();

    if (kind == MSG_READ)
    {
        *pbuf++ = 'R';
        *pbuf++ = ' ';
    }
    else if (kind == MSG_WRITE)
    {
        *pbuf++ = 'W';
        *pbuf++ = ' ';
    }

    for (i = 0; i < datalen; i++)
    {
        if (i > 15 && (i & 0xF) == 0)
            pbuf += sprintf(pbuf, "\n   ");
        pbuf += sprintf(pbuf, "%02X ", data[i]);
    }
    pbuf += sprintf(pbuf, "\n");
    dbgprint(buf.data());
}

void J2534::dbgprintptmsg(const PASSTHRU_MSG *pMsg, int kind)
{
    if (!pMsg)
    {
        dbgprint("(null message)\n");
        return;
    }
    dbgprint("ProtocolID=%lu,RxStatus=%lu,TxFlags=%lu,Timestamp=%lu,DataSize=%lu,ExtraDataIndex=%lu\n",
             pMsg->ProtocolID, pMsg->RxStatus, pMsg->TxFlags, pMsg->Timestamp, pMsg->DataSize, pMsg->ExtraDataIndex);
    dbgdump(pMsg->Data, pMsg->DataSize, kind);
}

long J2534::LoadJ2534DLL(const char *szDLL)
{
#if defined(OP20PT32_USE_LIB)
//    szDLL; // unused
//	if (!hDLL)
//		::OP20PT32_Start();
#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    hDLL = (HINSTANCE)1;
#else
    hDLL = (void *)1;
#endif
    getPTfns();
#else

    if (szDLL == nullptr)
    {
        DBGPRINT(("nullptr string pointer to J2534 DLL location.\n"));
        return (1);
    }

    FreeLibrary(hDLL);
    hDLL = nullptr;

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    if (!(hDLL = LoadLibraryA(szDLL)))
    {
        strcpy(lastError.data(), "error loading J2534 DLL");
        return false;
    }
    else if (!getPTfns())
    {
        // assume unusable if we don't have everything we need
        FreeLibrary(hDLL);
        hDLL = nullptr;
        strcpy(lastError.data(), "error loading J2534 DLL function pointers");
        return false;
    }
#else
    CFURLRef appUrlRef = CFBundleCopyBundleURL(CFBundleGetMainBundle());
    CFStringRef macPath = CFURLCopyFileSystemPath(appUrlRef, kCFURLPOSIXPathStyle);
    const char *pathPtr = CFStringGetCStringPtr(macPath, CFStringGetSystemEncoding());

    char libPath[1024];
    char oldPath[1024];

    getcwd(oldPath, 1024);
    strcpy(libPath, pathPtr);
    strcat(libPath, "/Contents/Frameworks");
    chdir(libPath); // change to this dir so J2534 .dylib can find any other needed dylibs in the same dir
    strcat(libPath,"/"");
    strcat(libPath,szDLL);

    CFRelease(appUrlRef);
    CFRelease(macPath);

    if (!(hDLL = dlopen(libPath, RTLD_LOCAL|RTLD_LAZY)))
    {
        strcpy(lastError.data(), "error loading ");
        strcat(lastError.data(), libPath);
        chdir(oldPath);
        return false;
    }
    else if (!getPTfns())
    {
        // assume unusable if we don't have everything we need
        dlclose(hDLL);
        hDLL = nullptr;
        strcpy(lastError.data(), "error loading J2534 dylib function pointers");
        chdir(oldPath);
        return false;
    }
    chdir(oldPath);
#endif

    DBGPRINT(("DLL loaded successfully\n"));
#endif
    return true;
}

bool J2534::checkDLL()
{
    if (bridgeClient)
        return bridgeClient->isRunning();
    if (hDLL)
        return true;

    bool is32Bit = false;
    if (isDll32Bit(dllName.data(), is32Bit) && is32Bit)
    {
        auto client = std::make_unique<J2534BridgeClient>("j2534_bridge_host.exe", dllName.data());
        if (client->start())
        {
            bridgeClient = std::move(client);
            useBridge = true;
            return true;
        }
        strcpy(lastError.data(), "error starting 32-bit J2534 bridge helper");
        return false;
    }

    LoadJ2534DLL(dllName.data());
    return (hDLL != nullptr);
}

bool J2534::is_serial_port_open()
{
    return J2534_init_ok;
}

long J2534::PassThruOpen(const void *pName, unsigned long *pDeviceID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruOpen(pName, pDeviceID);
    DBGPRINT(("PassThruOpen(name=%s,pDeviceID=@%08X)\n", (char *)pName, pDeviceID));

    result = (*pfPassThruOpen)(pName, pDeviceID);
    DBGPRINT(("PassThruOpen returned result %d and DeviceID %u\n", result, *pDeviceID));

    return result;
}

long J2534::PassThruClose(unsigned long DeviceID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruClose(DeviceID);
    DBGPRINT(("PassThruClose(%u)\n", DeviceID));
    result = (*pfPassThruClose)(DeviceID);
    DBGPRINT(("PassThruClose returned result %d\n", result));

    return result;
}

long J2534::PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags,
                            unsigned long Baudrate, unsigned long *pChannelID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruConnect(DeviceID, ProtocolID, Flags, Baudrate, pChannelID);
    DBGPRINT(("PassThruConnect(DeviceID=%u,ProtocolID=%u,Flags=%08X,Baudrate=%u,pChannelID=@%08X)\n", DeviceID,
              ProtocolID, Flags, Baudrate, pChannelID));
    result = (*pfPassThruConnect)(DeviceID, ProtocolID, Flags, Baudrate, pChannelID);
    DBGPRINT(("PassThruConnect returned result %d and ChannelID %u\n", result, *pChannelID));
    return result;
}

long J2534::PassThruDisconnect(unsigned long ChannelID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruDisconnect(ChannelID);
    DBGPRINT(("PassThruDisconnect(ChannelID=%u)\n", ChannelID));
    result = (*pfPassThruDisconnect)(ChannelID);
    DBGPRINT(("PassThruDisconnect returned result %d\n", result));
    return result;
}

long J2534::PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs,
                             unsigned long Timeout)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruReadMsgs(ChannelID, pMsg, pNumMsgs, Timeout);
    DBGPRINT(
        ("PassThruReadMsgs(ChannelID=%u,pMsg=@%08X,pNumMsgs=%u,Timeout=%u)\n", ChannelID, pMsg, *pNumMsgs, Timeout));
    result = (*pfPassThruReadMsgs)(ChannelID, pMsg, pNumMsgs, Timeout);
    DBGPRINT(("PassThruReadMsgs returned result %d\n", result));
    return result;
}

long J2534::PassThruWriteMsgs(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs,
                              unsigned long Timeout)
{
    unsigned int i;
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruWriteMsgs(ChannelID, pMsg, pNumMsgs, Timeout);
    DBGPRINT(
        ("PassThruWriteMsgs(ChannelID=%u,pMsg=@%08X,NumMsgs=%u,Timeout=%u)\n", ChannelID, pMsg, *pNumMsgs, Timeout));
    result = (*pfPassThruWriteMsgs)(ChannelID, pMsg, pNumMsgs, Timeout);
    for (i = 0; i < *pNumMsgs; i++)
        DBGPRINTPT((&(pMsg[i]), MSG_WRITE));
    DBGPRINT(("PassThruWriteMsgs returned result %d\n", result));
    return result;
}

long J2534::PassThruStartPeriodicMsg(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pMsgID,
                                     unsigned long TimeInterval)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruStartPeriodicMsg(ChannelID, pMsg, pMsgID, TimeInterval);
    DBGPRINT(("PassThruStartPeriodicMsg(ChannelID=%u,pMsg=@%08X,pMsgID=@%08X,TimeInterval=%u)\n", ChannelID, pMsg,
              pMsgID, TimeInterval));
    result = (*pfPassThruStartPeriodicMsg)(ChannelID, pMsg, pMsgID, TimeInterval);
    DBGPRINTPT((pMsg, 0));
    DBGPRINT(("PassThruStartPeriodicMsg returned result %d and MsgID %u\n", result, *pMsgID));
    return result;
}

long J2534::PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruStopPeriodicMsg(ChannelID, MsgID);
    DBGPRINT(("PassThruStopPeriodicMsg(ChannelID=%lu,MsgID=@%08lX)\n", ChannelID, MsgID));
    result = (*pfPassThruStopPeriodicMsg)(ChannelID, MsgID);
    DBGPRINT(("PassThruStopPeriodicMsg returned result %d\n", result));
    return result;
}

long J2534::PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType, const PASSTHRU_MSG *pMaskMsg,
                                   const PASSTHRU_MSG *pPatternMsg, const PASSTHRU_MSG *pFlowControlMsg,
                                   unsigned long *pMsgID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruStartMsgFilter(ChannelID, FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg,
                                                    pMsgID);
    DBGPRINT(("PassThruStartMsgFilter(ChannelID=%u,FilterType=%u,pMaskMsg=@%08X,pPatternMsg=@%08X,pFlowControlMsg=@%"
              "08X,pMsgID=@%08X)\n",
              ChannelID, FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg, pMsgID));
    DBGPRINT(("MaskMsg\n", result));
    DBGPRINTPT((pMaskMsg, 0));
    DBGPRINT(("PatternMsg\n", result));
    DBGPRINTPT((pPatternMsg, 0));
    DBGPRINT(("FlowControlMsg\n", result));
    DBGPRINTPT((pFlowControlMsg, 0));
    result = (*pfPassThruStartMsgFilter)(ChannelID, FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg, pMsgID);
    DBGPRINT(("PassThruStartMsgFilter returned result %d and MsgID %u\n", result, *pMsgID));
    return result;
}

long J2534::PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruStopMsgFilter(ChannelID, MsgID);
    DBGPRINT(("PassThruStopMsgFilter(ChannelID=%lu,MsgID=@%08lX)\n", ChannelID, MsgID));
    result = (*pfPassThruStopMsgFilter)(ChannelID, MsgID);
    DBGPRINT(("PassThruStopMsgFilter returned result %d\n", result));
    return result;
}

long J2534::PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin, unsigned long Voltage)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruSetProgrammingVoltage(DeviceID, Pin, Voltage);
    DBGPRINT(("PassThruSetProgrammingVoltage(DeviceID=%u,Pin=%u,Voltage=%u)\n", DeviceID, Pin, Voltage));
    result = (*pfPassThruSetProgrammingVoltage)(DeviceID, Pin, Voltage);
    DBGPRINT(("PassThruSetProgrammingVoltage returned result %d\n", result));
    return result;
}

long J2534::PassThruReadVersion(char *pApiVersion, char *pDllVersion, char *pFirmwareVersion, unsigned long DeviceID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruReadVersion(pApiVersion, pDllVersion, pFirmwareVersion, DeviceID);
    DBGPRINT(("PassThruReadVersion(DeviceID=%u,pFirmwareVersion=@%08X,pDllVersion=@%08X,pApiVersion=@%08X)\n", DeviceID,
              pFirmwareVersion, pDllVersion, pApiVersion));
    result = (*pfPassThruReadVersion)(DeviceID, pFirmwareVersion, pDllVersion, pApiVersion);
    DBGPRINT(("PassThruReadVersion returned result %d and FirmwareVersion [%s], DllVersion [%s], ApiVersion [%s]\n",
              result, pFirmwareVersion, pDllVersion, pApiVersion));
    return result;
}

long J2534::PassThruGetLastError(char *pErrorDescription)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruGetLastError(pErrorDescription);
    DBGPRINT(("PassThruGetLastError(pErrorDescription=@%08X\n", pErrorDescription));
    result = (*pfPassThruGetLastError)(pErrorDescription);
    DBGPRINT(("PassThruGetLastError returned result %d and ErrorDescription [%s]\n", result, pErrorDescription));
    return result;
}

int J2534::is_valid_sconfig_param(SCONFIG s)
{
    switch (s.Parameter)
    {
    case P1_MIN:
    case P2_MIN:
    case P3_MAX:
    case P4_MAX:
        return 0;
        break;
    default:
        return 1;
    }
}

void J2534::dump_sbyte_array(const SBYTE_ARRAY *s)
{
    DBGPRINT(("SBYTE_ARRAY size=%u\n", s->NumOfBytes));
    DBGDUMP((s->BytePtr, s->NumOfBytes, 0));
}

void J2534::dump_sconfig_param(SCONFIG s)
{
    std::array<char, 128> paramName;

    switch (s.Parameter)
    {
    case DATA_RATE:
        strcpy(paramName.data(), "DATA_RATE");
        break;
    case LOOPBACK:
        strcpy(paramName.data(), "LOOPBACK");
        break;
    case NODE_ADDRESS:
        strcpy(paramName.data(), "NODE_ADDRESS");
        break;
    case NETWORK_LINE:
        strcpy(paramName.data(), "NETWORK_LINE");
        break;
    case P1_MIN:
        strcpy(paramName.data(), "P1_MIN");
        break;
    case P1_MAX:
        strcpy(paramName.data(), "P1_MAX");
        break;
    case P2_MIN:
        strcpy(paramName.data(), "P2_MIN");
        break;
    case P2_MAX:
        strcpy(paramName.data(), "P2_MAX");
        break;
    case P3_MIN:
        strcpy(paramName.data(), "P3_MIN");
        break;
    case P3_MAX:
        strcpy(paramName.data(), "P3_MAX");
        break;
    case P4_MIN:
        strcpy(paramName.data(), "P4_MIN");
        break;
    case P4_MAX:
        strcpy(paramName.data(), "P4_MAX");
        break;
    case W1:
        strcpy(paramName.data(), "W1");
        break;
    case W2:
        strcpy(paramName.data(), "W2");
        break;
    case W3:
        strcpy(paramName.data(), "W3");
        break;
    case W4:
        strcpy(paramName.data(), "W4");
        break;
    case W5:
        strcpy(paramName.data(), "W5");
        break;
    case TIDLE:
        strcpy(paramName.data(), "TIDLE");
        break;
    case TINIL:
        strcpy(paramName.data(), "TINIL");
        break;
    case TWUP:
        strcpy(paramName.data(), "TWUP");
        break;
    case PARITY:
        strcpy(paramName.data(), "PARITY");
        break;
    case BIT_SAMPLE_POINT:
        strcpy(paramName.data(), "BIT_SAMPLE_POINT");
        break;
    case SYNC_JUMP_WIDTH:
        strcpy(paramName.data(), "SYNC_JUMP_WIDTH");
        break;
    case W0:
        strcpy(paramName.data(), "W0");
        break;
    case T1_MAX:
        strcpy(paramName.data(), "T1_MAX");
        break;
    case T2_MAX:
        strcpy(paramName.data(), "T2_MAX");
        break;
    case T4_MAX:
        strcpy(paramName.data(), "T4_MAX");
        break;
    case T5_MAX:
        strcpy(paramName.data(), "T5_MAX");
        break;
    case ISO15765_BS:
        strcpy(paramName.data(), "ISO15765_BS");
        break;
    case ISO15765_STMIN:
        strcpy(paramName.data(), "ISO15765_STMIN");
        break;
    case DATA_BITS:
        strcpy(paramName.data(), "DATA_BITS");
        break;
    case FIVE_BAUD_MOD:
        strcpy(paramName.data(), "FIVE_BAUD_MOD");
        break;
    case BS_TX:
        strcpy(paramName.data(), "BS_TX");
        break;
    case STMIN_TX:
        strcpy(paramName.data(), "STMIN_TX");
        break;
    case T3_MAX:
        strcpy(paramName.data(), "T3_MAX");
        break;
    case ISO15765_WFT_MAX:
        strcpy(paramName.data(), "ISO15765_WFT_MAX");
        break;
    case CAN_MIXED_FORMAT:
        strcpy(paramName.data(), "CAN_MIXED_FORMAT");
        break;
    case J1962_PINS:
        strcpy(paramName.data(), "J1962_PINS");
        break;
    case SW_CAN_HS_DATA_RATE:
        strcpy(paramName.data(), "W_CAN_HS_DATA_RATE");
        break;
    case SW_CAN_SPEEDCHANGE_ENABLE:
        strcpy(paramName.data(), "SW_CAN_SPEEDCHANGE_ENABLE");
        break;
    case SW_CAN_RES_SWITCH:
        strcpy(paramName.data(), "SW_CAN_RES_SWITCH");
        break;
    case ACTIVE_CHANNELS:
        strcpy(paramName.data(), "ACTIVE_CHANNELS");
        break;
    case SAMPLE_RATE:
        strcpy(paramName.data(), "SAMPLE_RATE");
        break;
    case SAMPLES_PER_READING:
        strcpy(paramName.data(), "SAMPLES_PER_READING");
        break;
    case READINGS_PER_MSG:
        strcpy(paramName.data(), "READINGS_PER_MSG");
        break;
    case AVERAGING_METHOD:
        strcpy(paramName.data(), "AVERAGING_METHOD");
        break;
    case SAMPLE_RESOLUTION:
        strcpy(paramName.data(), "SAMPLE_RESOLUTION");
        break;
    case INPUT_RANGE_LOW:
        strcpy(paramName.data(), "INPUT_RANGE_LOW");
        break;
    case INPUT_RANGE_HIGH:
        strcpy(paramName.data(), "INPUT_RANGE_HIGH");
        break;
    default:
        sprintf(paramName.data(), "%lu(unknown)", s.Parameter);
        break;
    }

    DBGPRINT(("    %s : %u", paramName.data(), s.Value));
}

long J2534::PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput)
{
    int input_as_sa = 0;
    int output_as_sa = 0;
    unsigned int i;
    SCONFIG_LIST *scl;
    long result = STATUS_NOERROR;
    std::array<char, 128> IoctlName;

    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruIoctl(ChannelID, IoctlID, pInput, pOutput);

    switch (IoctlID)
    {
    case GET_CONFIG:
        strcpy(IoctlName.data(), "GET_CONFIG");
        break;
    case SET_CONFIG:
        strcpy(IoctlName.data(), "SET_CONFIG");
        break;
    case READ_VBATT:
        strcpy(IoctlName.data(), "READ_VBATT");
        break;
    case FIVE_BAUD_INIT:
        strcpy(IoctlName.data(), "FIVE_BAUD_INIT");
        input_as_sa = 1;
        output_as_sa = 1;
        break;
    case FAST_INIT:
        strcpy(IoctlName.data(), "FAST_INIT");
        break;
    case CLEAR_TX_BUFFER:
        strcpy(IoctlName.data(), "CLEAR_TX_BUFFER");
        break;
    case CLEAR_RX_BUFFER:
        strcpy(IoctlName.data(), "CLEAR_RX_BUFFER");
        break;
    case CLEAR_PERIODIC_MSGS:
        strcpy(IoctlName.data(), "CLEAR_PERIODIC_MSGS");
        break;
    case CLEAR_MSG_FILTERS:
        strcpy(IoctlName.data(), "CLEAR_MSG_FILTERS");
        break;
    case CLEAR_FUNCT_MSG_LOOKUP_TABLE:
        strcpy(IoctlName.data(), "CLEAR_FUNCT_MSG_LOOKUP_TABLE");
        break;
    case ADD_TO_FUNCT_MSG_LOOKUP_TABLE:
        strcpy(IoctlName.data(), "ADD_TO_FUNCT_MSG_LOOKUP_TABLE");
        break;
    case DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE:
        strcpy(IoctlName.data(), "DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE");
        break;
    case READ_PROG_VOLTAGE:
        strcpy(IoctlName.data(), "READ_PROG_VOLTAGE");
        break;
        //    case TX_IOCTL_APP_SERVICE:
        //        strcpy(IoctlName.data(),"APP_SERVICE");
        //        break;
    default:
        sprintf(IoctlName.data(), "%lu(unknown)", IoctlID);
        break;
    }

    DBGPRINT(("PassThruIoctl(ChannelID=%u,Ioctl=%s,pInput=@%08X,pOutputMsgID=@%08X)\n", ChannelID, IoctlName.data(),
              pInput, pOutput));

    if (IoctlID == SET_CONFIG)
    {
        pOutput = nullptr; // make some DLLs happy

        // dump params
        scl = (SCONFIG_LIST *)pInput;
        for (i = 0; i < scl->NumOfParams; i++)
            dump_sconfig_param((scl->ConfigPtr)[i]);

        // Enabling this could break some J2534 devices such as Denso DST-i
        /*for (i = 0; i < scl->NumOfParams; i++)
            if (!is_valid_sconfig_param((scl->ConfigPtr)[i]))
            {
                DBGPRINT(("param not allowed - not passing through and instead faking success\n",result));
                return STATUS_NOERROR;
            }*/
    }

    if (input_as_sa)
    {
        DBGPRINT(("Input\n"));
        dump_sbyte_array((SBYTE_ARRAY *)pInput);
    }

    result = (*pfPassThruIoctl)(ChannelID, IoctlID, pInput, pOutput);

    if (output_as_sa)
    {
        DBGPRINT(("Output\n"));
        dump_sbyte_array((SBYTE_ARRAY *)pOutput);
    }

    DBGPRINT(("PassThruIoctl returned result %d\n", result));
    return result;
}
