#include "J2534_tactrix_win.h"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace
{

// SAE J2534-1 sizes every version and error-description buffer at 80 chars.
constexpr std::size_t kJ2534TextBufferSize = 80;

void copyJ2534Text(char *out, std::string_view text)
{
    const std::size_t length = std::min(text.size(), kJ2534TextBufferSize - 1);
    std::ranges::copy(text.substr(0, length), out);
    out[length] = '\0';
}

} // namespace

extern "C"
{

    __declspec(dllexport) long PT_CALL PassThruOpen(const void * /*pName*/, unsigned long *p_device_id)
    {
        *p_device_id = 7;
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruClose(unsigned long /*DeviceID*/)
    {
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruConnect(unsigned long /*DeviceID*/, unsigned long /*ProtocolID*/,
                                                       unsigned long /*Flags*/, unsigned long /*Baudrate*/,
                                                       unsigned long *p_channel_id)
    {
        *p_channel_id = 3;
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruDisconnect(unsigned long /*ChannelID*/)
    {
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruReadMsgs(unsigned long /*ChannelID*/, PassThruMsg *p_msg,
                                                        unsigned long *p_num_msgs, unsigned long /*Timeout*/)
    {
        p_msg[0].data_size = 4;
        p_msg[0].data[0] = 0xDE;
        p_msg[0].data[1] = 0xAD;
        p_msg[0].data[2] = 0xBE;
        p_msg[0].data[3] = 0xEF;
        *p_num_msgs = 1;
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruWriteMsgs(unsigned long /*ChannelID*/, const PassThruMsg *p_msg,
                                                         unsigned long *p_num_msgs, unsigned long /*Timeout*/)
    {
        *p_num_msgs = 1;
        return (p_msg[0].data_size > 0 && p_msg[0].data[0] == 0x11) ? kJ2534StatusNoerror : kJ2534ErrFailed;
    }

    __declspec(dllexport) long PT_CALL PassThruStartPeriodicMsg(unsigned long /*ChannelID*/,
                                                                const PassThruMsg * /*pMsg*/, unsigned long *p_msg_id,
                                                                unsigned long /*TimeInterval*/)
    {
        *p_msg_id = 55;
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruStopPeriodicMsg(unsigned long /*ChannelID*/, unsigned long /*MsgID*/)
    {
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruStartMsgFilter(unsigned long /*ChannelID*/, unsigned long /*FilterType*/,
                                                              const void * /*pMaskMsg*/, const void * /*pPatternMsg*/,
                                                              const void * /*pFlowControlMsg*/, unsigned long *p_msg_id)
    {
        *p_msg_id = 99;
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruStopMsgFilter(unsigned long /*ChannelID*/, unsigned long /*MsgID*/)
    {
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruSetProgrammingVoltage(unsigned long /*DeviceID*/, unsigned long /*Pin*/,
                                                                     unsigned long /*Voltage*/)
    {
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruReadVersion(unsigned long /*DeviceID*/, char *p_api_version,
                                                           char *p_dll_version, char *p_firmware_version)
    {
        copyJ2534Text(p_api_version, "04.04");
        copyJ2534Text(p_dll_version, "1.0.0-fake");
        copyJ2534Text(p_firmware_version, "0.0.0-fake");
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruGetLastError(char *p_error_description)
    {
        copyJ2534Text(p_error_description, "fake DLL error");
        return kJ2534StatusNoerror;
    }

    __declspec(dllexport) long PT_CALL PassThruIoctl(unsigned long /*ChannelID*/, unsigned long ioctl_id,
                                                     const void * /*pInput*/, void *p_output)
    {
        if (ioctl_id == kJ2534ReadVbatt)
        {
            *static_cast<unsigned long *>(p_output) = 12500;
        }
        return kJ2534StatusNoerror;
    }

} // extern "C"
