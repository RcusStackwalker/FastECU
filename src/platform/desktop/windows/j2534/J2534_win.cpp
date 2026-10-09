#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include "src/platform/desktop/windows/j2534/J2534_win.h"
#include "src/platform/desktop/windows/j2534/pe_bitness.h"
#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#else
#include <dlfcn.h>
#include <CoreFoundation/CFBundle.h>
#include <unistd.h>
#endif

namespace
{

// Copies `text` into `out`, truncating it to fit, and always NUL-terminates.
void copyCString(std::span<char> out, std::string_view text)
{
    const std::size_t length = std::min(text.size(), out.size() - 1);
    std::ranges::copy(text.substr(0, length), out.begin());
    out[length] = '\0';
}

} // namespace

J2534::J2534()
{
    h_dll_ = nullptr;
    is_library_initialized_ = false;
    // default to the Openport 2.0 J2534 DLL
#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    copyCString(dll_name_, "j2534.dll");
#else
    copyCString(dll_name_, "op20pt32.dylib");
#endif
}

void J2534::setDllName(const char *name)
{
    copyCString(dll_name_, name);
}

void J2534::getDllName(char *name)
{
    // `name` must hold dll_name_.size() chars, as the one caller's buffer does.
    copyCString(std::span(name, dll_name_.size()), dll_name_.data());
}

void J2534::disable()
{
    if (h_dll_)
    {
        FreeLibrary(h_dll_);
        h_dll_ = nullptr;
    }
    if (bridge_client_)
    {
        bridge_client_.reset();
        use_bridge_ = false;
    }
}

char *J2534::getLastError()
{
    return last_error_.data();
}

bool J2534::valid()
{
    if (bridge_client_)
    {
        return bridge_client_->isRunning();
    }
    return h_dll_ != nullptr;
}

J2534::~J2534()
{
#if defined(OP20PT32_USE_LIB)
    //	if (h_dll_)
    //		::OP20PT32_Stop();
    // #else

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    if (h_dll_)
        FreeLibrary(h_dll_);
#else
    if (h_dll_)
        dlclose(h_dll_);
#endif
#endif
}

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
#if defined(OP20PT32_USE_LIB)
#define getPTfn(name) pf##name = ::name;
#else
#define getPTfn(name)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        pf##name = (Pf##name *)GetProcAddress(h_dll_, "" #name);                                                       \
        if (!pf##name)                                                                                                 \
        {                                                                                                              \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)
#endif
#else
#if defined(OP20PT32_USE_LIB)
#define getPTfn(name) pf##name = ::name;
#else
#define getPTfn(name)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        pf##name = (Pf##name *)dlsym(h_dll_, "" #name);                                                                \
        if (!pf##name)                                                                                                 \
        {                                                                                                              \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)
#endif
#endif

bool J2534::getPTfns()
{
    if (!h_dll_)
    {
        return false;
    }

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
//	if (!h_dll_)
//		::OP20PT32_Start();
#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    h_dll_ = (HINSTANCE)1;
#else
    h_dll_ = (void *)1;
#endif
    getPTfns();
#else

    if (szDLL == nullptr)
    {
        return (1);
    }

    FreeLibrary(h_dll_);
    h_dll_ = nullptr;

#if defined(_WIN32) || defined(WIN32) || defined(_WIN64) || defined(WIN64)
    h_dll_ = LoadLibraryA(szDLL);
    if (!h_dll_)
    {
        copyCString(last_error_, "error loading J2534 DLL");
        return false;
    }
    else if (!getPTfns())
    {
        // assume unusable if we don't have everything we need
        FreeLibrary(h_dll_);
        h_dll_ = nullptr;
        copyCString(last_error_, "error loading J2534 DLL function pointers");
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

    if (!(h_dll_ = dlopen(libPath, RTLD_LOCAL|RTLD_LAZY)))
    {
        strcpy(last_error_.data(), "error loading ");
        strcat(last_error_.data(), libPath);
        chdir(oldPath);
        return false;
    }
    else if (!getPTfns())
    {
        // assume unusable if we don't have everything we need
        dlclose(h_dll_);
        h_dll_ = nullptr;
        strcpy(last_error_.data(), "error loading J2534 dylib function pointers");
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
    if (bridge_client_)
    {
        return bridge_client_->isRunning();
    }
    if (h_dll_)
    {
        return true;
    }

    bool is32Bit = false;
    if (isDll32Bit(dll_name_.data(), is32Bit) && is32Bit)
    {
        auto client = std::make_unique<J2534BridgeClient>("j2534_bridge_host.exe", dll_name_.data());
        if (client->start())
        {
            bridge_client_ = std::move(client);
            use_bridge_ = true;
            return true;
        }
        copyCString(last_error_, "error starting 32-bit J2534 bridge helper");
        return false;
    }

    LoadJ2534DLL(dll_name_.data());
    return (h_dll_ != nullptr);
}

bool J2534::is_serial_port_open()
{
    return j2534_init_ok;
}

long J2534::PassThruOpen(const void *pName, unsigned long *pDeviceID)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruOpen(pName, pDeviceID);
    }

    result = (*pfPassThruOpen)(pName, pDeviceID);

    return result;
}

long J2534::PassThruClose(unsigned long DeviceID)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruClose(DeviceID);
    }
    result = (*pfPassThruClose)(DeviceID);

    return result;
}

long J2534::PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags,
                            unsigned long Baudrate, unsigned long *pChannelID)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruConnect(DeviceID, ProtocolID, Flags, Baudrate, pChannelID);
    }
    result = (*pfPassThruConnect)(DeviceID, ProtocolID, Flags, Baudrate, pChannelID);
    return result;
}

long J2534::PassThruDisconnect(unsigned long ChannelID)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruDisconnect(ChannelID);
    }
    result = (*pfPassThruDisconnect)(ChannelID);
    return result;
}

long J2534::PassThruReadMsgs(unsigned long ChannelID, PassThruMsg *pMsg, unsigned long *pNumMsgs, unsigned long Timeout)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruReadMsgs(ChannelID, pMsg, pNumMsgs, Timeout);
    }
    result = (*pfPassThruReadMsgs)(ChannelID, pMsg, pNumMsgs, Timeout);
    return result;
}

long J2534::PassThruWriteMsgs(unsigned long ChannelID, const PassThruMsg *pMsg, unsigned long *pNumMsgs,
                              unsigned long Timeout)
{
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruWriteMsgs(ChannelID, pMsg, pNumMsgs, Timeout);
    }
    return (*pfPassThruWriteMsgs)(ChannelID, pMsg, pNumMsgs, Timeout);
}

long J2534::PassThruStartPeriodicMsg(unsigned long ChannelID, const PassThruMsg *pMsg, unsigned long *pMsgID,
                                     unsigned long TimeInterval)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruStartPeriodicMsg(ChannelID, pMsg, pMsgID, TimeInterval);
    }
    result = (*pfPassThruStartPeriodicMsg)(ChannelID, pMsg, pMsgID, TimeInterval);
    return result;
}

long J2534::PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruStopPeriodicMsg(ChannelID, MsgID);
    }
    result = (*pfPassThruStopPeriodicMsg)(ChannelID, MsgID);
    return result;
}

long J2534::PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType, const PassThruMsg *pMaskMsg,
                                   const PassThruMsg *pPatternMsg, const PassThruMsg *pFlowControlMsg,
                                   unsigned long *pMsgID)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruStartMsgFilter(ChannelID, FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg,
                                                      pMsgID);
    }
    result = (*pfPassThruStartMsgFilter)(ChannelID, FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg, pMsgID);
    return result;
}

long J2534::PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruStopMsgFilter(ChannelID, MsgID);
    }
    result = (*pfPassThruStopMsgFilter)(ChannelID, MsgID);
    return result;
}

long J2534::PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin, unsigned long Voltage)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruSetProgrammingVoltage(DeviceID, Pin, Voltage);
    }
    result = (*pfPassThruSetProgrammingVoltage)(DeviceID, Pin, Voltage);
    return result;
}

long J2534::PassThruReadVersion(char *pApiVersion, char *pDllVersion, char *pFirmwareVersion, unsigned long DeviceID)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruReadVersion(pApiVersion, pDllVersion, pFirmwareVersion, DeviceID);
    }
    result = (*pfPassThruReadVersion)(DeviceID, pFirmwareVersion, pDllVersion, pApiVersion);
    return result;
}

long J2534::PassThruGetLastError(char *pErrorDescription)
{
    long result = kJ2534StatusNoerror;
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruGetLastError(pErrorDescription);
    }
    result = (*pfPassThruGetLastError)(pErrorDescription);
    return result;
}

int J2534::is_valid_sconfig_param(SCONFIG s)
{
    switch (s.parameter)
    {
    case kJ2534P1Min:
    case kJ2534P2Min:
    case kJ2534P3Max:
    case kJ2534P4Max:
        return 0;
        break;
    default:
        return 1;
    }
}

long J2534::PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput)
{
    if (!checkDLL())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruIoctl(ChannelID, IoctlID, pInput, pOutput);
    }

    if (IoctlID == kJ2534SetConfig)
    {
        pOutput = nullptr; // make some DLLs happy
    }

    // Parked, not dropped: rejecting parameters the DLL does not accept could
    // break some J2534 devices such as the Denso DST-i, so every SET_CONFIG
    // parameter is passed through as-is. is_valid_sconfig_param() classifies
    // them if this is ever revisited.
    //     const auto *scl = static_cast<const SConfigList *>(pInput);
    //     for (unsigned i = 0; i < scl->num_of_params; i++)
    //         if (!is_valid_sconfig_param((scl->config_ptr)[i]))
    //             return STATUS_NOERROR;

    return (*pfPassThruIoctl)(ChannelID, IoctlID, pInput, pOutput);
}
