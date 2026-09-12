#include <array>
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

    result = (*pfPassThruOpen)(pName, pDeviceID);

    return result;
}

long J2534::PassThruClose(unsigned long DeviceID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruClose(DeviceID);
    result = (*pfPassThruClose)(DeviceID);

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
    result = (*pfPassThruConnect)(DeviceID, ProtocolID, Flags, Baudrate, pChannelID);
    return result;
}

long J2534::PassThruDisconnect(unsigned long ChannelID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruDisconnect(ChannelID);
    result = (*pfPassThruDisconnect)(ChannelID);
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
    result = (*pfPassThruReadMsgs)(ChannelID, pMsg, pNumMsgs, Timeout);
    return result;
}

long J2534::PassThruWriteMsgs(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs,
                              unsigned long Timeout)
{
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruWriteMsgs(ChannelID, pMsg, pNumMsgs, Timeout);
    return (*pfPassThruWriteMsgs)(ChannelID, pMsg, pNumMsgs, Timeout);
}

long J2534::PassThruStartPeriodicMsg(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pMsgID,
                                     unsigned long TimeInterval)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruStartPeriodicMsg(ChannelID, pMsg, pMsgID, TimeInterval);
    result = (*pfPassThruStartPeriodicMsg)(ChannelID, pMsg, pMsgID, TimeInterval);
    return result;
}

long J2534::PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruStopPeriodicMsg(ChannelID, MsgID);
    result = (*pfPassThruStopPeriodicMsg)(ChannelID, MsgID);
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
    result = (*pfPassThruStartMsgFilter)(ChannelID, FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg, pMsgID);
    return result;
}

long J2534::PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruStopMsgFilter(ChannelID, MsgID);
    result = (*pfPassThruStopMsgFilter)(ChannelID, MsgID);
    return result;
}

long J2534::PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin, unsigned long Voltage)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruSetProgrammingVoltage(DeviceID, Pin, Voltage);
    result = (*pfPassThruSetProgrammingVoltage)(DeviceID, Pin, Voltage);
    return result;
}

long J2534::PassThruReadVersion(char *pApiVersion, char *pDllVersion, char *pFirmwareVersion, unsigned long DeviceID)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruReadVersion(pApiVersion, pDllVersion, pFirmwareVersion, DeviceID);
    result = (*pfPassThruReadVersion)(DeviceID, pFirmwareVersion, pDllVersion, pApiVersion);
    return result;
}

long J2534::PassThruGetLastError(char *pErrorDescription)
{
    long result = STATUS_NOERROR;
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruGetLastError(pErrorDescription);
    result = (*pfPassThruGetLastError)(pErrorDescription);
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

long J2534::PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput)
{
    if (!checkDLL())
        return ERR_DEVICE_NOT_CONNECTED;
    if (useBridge)
        return bridgeClient->PassThruIoctl(ChannelID, IoctlID, pInput, pOutput);

    if (IoctlID == SET_CONFIG)
        pOutput = nullptr; // make some DLLs happy

    // Parked, not dropped: rejecting parameters the DLL does not accept could
    // break some J2534 devices such as the Denso DST-i, so every SET_CONFIG
    // parameter is passed through as-is. is_valid_sconfig_param() classifies
    // them if this is ever revisited.
    //     const auto *scl = static_cast<const SCONFIG_LIST *>(pInput);
    //     for (unsigned i = 0; i < scl->NumOfParams; i++)
    //         if (!is_valid_sconfig_param((scl->ConfigPtr)[i]))
    //             return STATUS_NOERROR;

    return (*pfPassThruIoctl)(ChannelID, IoctlID, pInput, pOutput);
}
