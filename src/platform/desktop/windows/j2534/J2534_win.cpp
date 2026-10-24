#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include "src/platform/desktop/windows/j2534/J2534_win.h"
#include "src/platform/desktop/windows/j2534/pe_bitness.h"

namespace
{

// Copies `text` into `out`, truncating it to fit, and always NUL-terminates.
void CopyCString(std::span<char> out, std::string_view text)
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
    CopyCString(dll_name_, "j2534.dll");
}

void J2534::SetDllName(const char *name)
{
    CopyCString(dll_name_, name);
}

void J2534::GetDllName(char *name)
{
    // `name` must hold dll_name_.size() chars, as the one caller's buffer does.
    CopyCString(std::span(name, dll_name_.size()), dll_name_.data());
}

void J2534::Disable()
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

bool J2534::Valid()
{
    if (bridge_client_)
    {
        return bridge_client_->IsRunning();
    }
    return h_dll_ != nullptr;
}

J2534::~J2534()
{
    if (h_dll_)
    {
        FreeLibrary(h_dll_);
    }
}

#define GET_PT_FN(name)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        pf##name = (Pf##name *)GetProcAddress(h_dll_, "" #name);                                                       \
        if (!pf##name)                                                                                                 \
        {                                                                                                              \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

bool J2534::GetPTfns()
{
    if (!h_dll_)
    {
        return false;
    }

    GET_PT_FN(PassThruOpen);
    GET_PT_FN(PassThruClose);
    GET_PT_FN(PassThruConnect);
    GET_PT_FN(PassThruDisconnect);
    GET_PT_FN(PassThruReadMsgs);
    GET_PT_FN(PassThruWriteMsgs);
    GET_PT_FN(PassThruStartPeriodicMsg);
    GET_PT_FN(PassThruStopPeriodicMsg);
    GET_PT_FN(PassThruStartMsgFilter);
    GET_PT_FN(PassThruStopMsgFilter);
    GET_PT_FN(PassThruSetProgrammingVoltage);
    GET_PT_FN(PassThruReadVersion);
    GET_PT_FN(PassThruGetLastError);
    GET_PT_FN(PassThruIoctl);

    return true;
}

long J2534::LoadJ2534DLL(const char *sz_dll)
{
    if (sz_dll == nullptr)
    {
        return (1);
    }

    FreeLibrary(h_dll_);
    h_dll_ = nullptr;

    h_dll_ = LoadLibraryA(sz_dll);
    if (!h_dll_)
    {
        return false;
    }
    else if (!GetPTfns())
    {
        // assume unusable if we don't have everything we need
        FreeLibrary(h_dll_);
        h_dll_ = nullptr;
        return false;
    }
    return true;
}

bool J2534::CheckDll()
{
    if (bridge_client_)
    {
        return bridge_client_->IsRunning();
    }
    if (h_dll_)
    {
        return true;
    }

    bool is32_bit = false;
    if (IsDll32Bit(dll_name_.data(), is32_bit) && is32_bit)
    {
        auto client = std::make_unique<J2534BridgeClient>("j2534_bridge_host.exe", dll_name_.data());
        if (client->Start())
        {
            bridge_client_ = std::move(client);
            use_bridge_ = true;
            return true;
        }
        return false;
    }

    LoadJ2534DLL(dll_name_.data());
    return (h_dll_ != nullptr);
}

bool J2534::IsSerialPortOpen()
{
    return j2534_init_ok;
}

long J2534::PassThruOpen(const void *p_name, unsigned long *p_device_id)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruOpen(p_name, p_device_id);
    }

    result = (*pfPassThruOpen)(p_name, p_device_id);

    return result;
}

long J2534::PassThruClose(unsigned long device_id)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruClose(device_id);
    }
    result = (*pfPassThruClose)(device_id);

    return result;
}

long J2534::PassThruConnect(unsigned long device_id, unsigned long protocol_id, unsigned long flags,
                            unsigned long baudrate, unsigned long *p_channel_id)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruConnect(device_id, protocol_id, flags, baudrate, p_channel_id);
    }
    result = (*pfPassThruConnect)(device_id, protocol_id, flags, baudrate, p_channel_id);
    return result;
}

long J2534::PassThruDisconnect(unsigned long channel_id)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruDisconnect(channel_id);
    }
    result = (*pfPassThruDisconnect)(channel_id);
    return result;
}

long J2534::PassThruReadMsgs(unsigned long channel_id, PassThruMsg *p_msg, unsigned long *p_num_msgs,
                             unsigned long timeout)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruReadMsgs(channel_id, p_msg, p_num_msgs, timeout);
    }
    result = (*pfPassThruReadMsgs)(channel_id, p_msg, p_num_msgs, timeout);
    return result;
}

long J2534::PassThruWriteMsgs(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_num_msgs,
                              unsigned long timeout)
{
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruWriteMsgs(channel_id, p_msg, p_num_msgs, timeout);
    }
    return (*pfPassThruWriteMsgs)(channel_id, p_msg, p_num_msgs, timeout);
}

long J2534::PassThruStartPeriodicMsg(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_msg_id,
                                     unsigned long time_interval)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruStartPeriodicMsg(channel_id, p_msg, p_msg_id, time_interval);
    }
    result = (*pfPassThruStartPeriodicMsg)(channel_id, p_msg, p_msg_id, time_interval);
    return result;
}

long J2534::PassThruStopPeriodicMsg(unsigned long channel_id, unsigned long msg_id)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruStopPeriodicMsg(channel_id, msg_id);
    }
    result = (*pfPassThruStopPeriodicMsg)(channel_id, msg_id);
    return result;
}

long J2534::PassThruStartMsgFilter(unsigned long channel_id, unsigned long filter_type, const PassThruMsg *p_mask_msg,
                                   const PassThruMsg *p_pattern_msg, const PassThruMsg *p_flow_control_msg,
                                   unsigned long *p_msg_id)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruStartMsgFilter(channel_id, filter_type, p_mask_msg, p_pattern_msg,
                                                      p_flow_control_msg, p_msg_id);
    }
    result =
        (*pfPassThruStartMsgFilter)(channel_id, filter_type, p_mask_msg, p_pattern_msg, p_flow_control_msg, p_msg_id);
    return result;
}

long J2534::PassThruStopMsgFilter(unsigned long channel_id, unsigned long msg_id)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruStopMsgFilter(channel_id, msg_id);
    }
    result = (*pfPassThruStopMsgFilter)(channel_id, msg_id);
    return result;
}

long J2534::PassThruSetProgrammingVoltage(unsigned long device_id, unsigned long pin, unsigned long voltage)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruSetProgrammingVoltage(device_id, pin, voltage);
    }
    result = (*pfPassThruSetProgrammingVoltage)(device_id, pin, voltage);
    return result;
}

long J2534::PassThruReadVersion(char *p_api_version, char *p_dll_version, char *p_firmware_version,
                                unsigned long device_id)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruReadVersion(p_api_version, p_dll_version, p_firmware_version, device_id);
    }
    result = (*pfPassThruReadVersion)(device_id, p_firmware_version, p_dll_version, p_api_version);
    return result;
}

long J2534::PassThruGetLastError(char *p_error_description)
{
    long result = kJ2534StatusNoerror;
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruGetLastError(p_error_description);
    }
    result = (*pfPassThruGetLastError)(p_error_description);
    return result;
}

int J2534::IsValidSconfigParam(SCONFIG s)
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

long J2534::PassThruIoctl(unsigned long channel_id, unsigned long ioctl_id, const void *p_input, void *p_output)
{
    if (!CheckDll())
    {
        return kJ2534ErrDeviceNotConnected;
    }
    if (use_bridge_)
    {
        return bridge_client_->PassThruIoctl(channel_id, ioctl_id, p_input, p_output);
    }

    if (ioctl_id == kJ2534SetConfig)
    {
        p_output = nullptr; // make some DLLs happy
    }

    // Parked, not dropped: rejecting parameters the DLL does not accept could
    // break some J2534 devices such as the Denso DST-i, so every SET_CONFIG
    // parameter is passed through as-is. is_valid_sconfig_param() classifies
    // them if this is ever revisited.
    //     const auto *scl = static_cast<const SConfigList *>(pInput);
    //     for (unsigned i = 0; i < scl->num_of_params; i++)
    //         if (!is_valid_sconfig_param((scl->config_ptr)[i]))
    //             return STATUS_NOERROR;

    return (*pfPassThruIoctl)(channel_id, ioctl_id, p_input, p_output);
}
