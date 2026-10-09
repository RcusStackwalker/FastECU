#include "src/platform/desktop/windows/j2534/j2534_bridge_client.h"
#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

#include <array>
#include <cstring>
#include <tuple>
#include <vector>

using namespace j2534_bridge;

namespace
{

// Reads a fixed-size Resp payload and confirms the frame's Function tag
// matches what this call expects. The wire protocol is strict request/
// response ping-pong (one outstanding request at a time, never pipelined),
// and every host-side handler echoes back the same Function tag it was
// dispatched on -- including the size-mismatch error path in
// j2534_bridge_host/main.cpp's readTypedRequest(), which still responds
// with the correctly-tagged, correctly-sized Resp. So in a version-matched
// client/host pair the tag can never actually mismatch. We still check it:
// readFrame() already guards against an oversized payload overflowing our
// buffer, but a smaller-than-expected (mismatched-struct) payload would
// pass that guard and silently leave the tail of resp at its zero-init
// value. Catching a wrong tag turns that class of protocol/version-skew
// bug into a clean ERR_FAILED instead of quietly-wrong data.
template <typename Resp> bool readAndValidateResponse(HANDLE pipe, Function expectedFunction, Resp& resp)
{
    FrameHeader header{};
    if (!readFrame(pipe, header, &resp, sizeof(resp)))
    {
        return false;
    }
    return header.function == expectedFunction;
}

} // namespace

J2534BridgeClient::J2534BridgeClient(std::string hostExePath, std::string vendorDllPath)
    : host_exe_path_(std::move(hostExePath)), vendor_dll_path_(std::move(vendorDllPath))
{
}

J2534BridgeClient::~J2534BridgeClient()
{
    stop();
}

bool J2534BridgeClient::start()
{
    HANDLE childStdinRead, childStdinWrite, childStdoutRead, childStdoutWrite;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

    if (!CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0))
    {
        return false;
    }
    if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0))
    {
        CloseHandle(childStdinRead);
        CloseHandle(childStdinWrite);
        return false;
    }
    SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childStdinRead;
    si.hStdOutput = childStdoutWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::string cmdLine = "\"" + host_exe_path_ + "\" \"" + vendor_dll_path_ + "\"";
    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cmdLineBuf.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                             &si, &process_info_);
    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);
    if (!ok)
    {
        CloseHandle(childStdinWrite);
        CloseHandle(childStdoutRead);
        return false;
    }

    // Job Object: if this process dies or is killed, Windows tears down the
    // helper too -- no orphaned bridge process left holding the adapter open.
    job_object_ = CreateJobObjectA(nullptr, nullptr);
    if (job_object_)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_object_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job_object_, process_info_.hProcess);
    }
    ResumeThread(process_info_.hThread);

    to_child_write_ = childStdinWrite;
    from_child_read_ = childStdoutRead;
    running_ = true;
    return true;
}

void J2534BridgeClient::stop()
{
    if (!running_)
    {
        return;
    }
    writeFrame(to_child_write_, Function::kShutdown, nullptr, 0);
    WaitForSingleObject(process_info_.hProcess, 2000);
    CloseHandle(to_child_write_);
    CloseHandle(from_child_read_);
    CloseHandle(process_info_.hProcess);
    CloseHandle(process_info_.hThread);
    if (job_object_)
    {
        CloseHandle(job_object_);
    }
    running_ = false;
}

long J2534BridgeClient::PassThruOpen(const void *pName, unsigned long *pDeviceID)
{
    PassThruOpenRequest req{};
    if (pName)
    {
        req.has_name = true;
        strncpy_s(req.name.data(), req.name.size(), static_cast<const char *>(pName), req.name.size() - 1);
    }
    if (!writeFrame(to_child_write_, Function::kPassThruOpen, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruOpenResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruOpen, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    *pDeviceID = resp.device_id;
    return resp.result;
}

long J2534BridgeClient::PassThruClose(unsigned long DeviceID)
{
    PassThruCloseRequest req{DeviceID};
    if (!writeFrame(to_child_write_, Function::kPassThruClose, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruCloseResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruClose, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags,
                                        unsigned long Baudrate, unsigned long *pChannelID)
{
    PassThruConnectRequest req{DeviceID, ProtocolID, Flags, Baudrate};
    if (!writeFrame(to_child_write_, Function::kPassThruConnect, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruConnectResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruConnect, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    *pChannelID = resp.channel_id;
    return resp.result;
}

long J2534BridgeClient::PassThruDisconnect(unsigned long ChannelID)
{
    PassThruDisconnectRequest req{ChannelID};
    if (!writeFrame(to_child_write_, Function::kPassThruDisconnect, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruDisconnectResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruDisconnect, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruReadMsgs(unsigned long ChannelID, PassThruMsg *pMsg, unsigned long *pNumMsgs,
                                         unsigned long Timeout)
{
    PassThruReadMsgsRequest req{ChannelID, Timeout};
    if (!writeFrame(to_child_write_, Function::kPassThruReadMsgs, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruReadMsgsResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruReadMsgs, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    *pMsg = resp.msg;
    *pNumMsgs = resp.num_msgs;
    return resp.result;
}

long J2534BridgeClient::PassThruWriteMsgs(unsigned long ChannelID, const PassThruMsg *pMsg, unsigned long *pNumMsgs,
                                          unsigned long Timeout)
{
    PassThruWriteMsgsRequest req{};
    req.channel_id = ChannelID;
    req.timeout = Timeout;
    if (pMsg)
    {
        req.msg = *pMsg;
    }
    if (!writeFrame(to_child_write_, Function::kPassThruWriteMsgs, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruWriteMsgsResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruWriteMsgs, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    if (pNumMsgs)
    {
        *pNumMsgs = resp.num_msgs;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruStartPeriodicMsg(unsigned long ChannelID, const PassThruMsg *pMsg,
                                                 unsigned long *pMsgID, unsigned long TimeInterval)
{
    PassThruStartPeriodicMsgRequest req{};
    req.channel_id = ChannelID;
    req.time_interval = TimeInterval;
    if (pMsg)
    {
        req.msg = *pMsg;
    }
    if (!writeFrame(to_child_write_, Function::kPassThruStartPeriodicMsg, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruStartPeriodicMsgResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruStartPeriodicMsg, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    *pMsgID = resp.msg_id;
    return resp.result;
}

long J2534BridgeClient::PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    PassThruStopPeriodicMsgRequest req{ChannelID, MsgID};
    if (!writeFrame(to_child_write_, Function::kPassThruStopPeriodicMsg, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruStopPeriodicMsgResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruStopPeriodicMsg, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType,
                                               const PassThruMsg *pMaskMsg, const PassThruMsg *pPatternMsg,
                                               const PassThruMsg *pFlowControlMsg, unsigned long *pMsgID)
{
    PassThruStartMsgFilterRequest req{};
    req.channel_id = ChannelID;
    req.filter_type = FilterType;
    if (pMaskMsg)
    {
        req.mask_msg = *pMaskMsg;
    }
    if (pPatternMsg)
    {
        req.pattern_msg = *pPatternMsg;
    }
    req.has_flow_control_msg = (pFlowControlMsg != nullptr);
    if (pFlowControlMsg)
    {
        req.flow_control_msg = *pFlowControlMsg;
    }
    if (!writeFrame(to_child_write_, Function::kPassThruStartMsgFilter, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruStartMsgFilterResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruStartMsgFilter, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    *pMsgID = resp.msg_id;
    return resp.result;
}

long J2534BridgeClient::PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID)
{
    PassThruStopMsgFilterRequest req{ChannelID, MsgID};
    if (!writeFrame(to_child_write_, Function::kPassThruStopMsgFilter, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruStopMsgFilterResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruStopMsgFilter, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin, unsigned long Voltage)
{
    PassThruSetProgrammingVoltageRequest req{DeviceID, Pin, Voltage};
    if (!writeFrame(to_child_write_, Function::kPassThruSetProgrammingVoltage, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruSetProgrammingVoltageResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruSetProgrammingVoltage, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruReadVersion(char *pApiVersion, char *pDllVersion, char *pFirmwareVersion,
                                            unsigned long DeviceID)
{
    PassThruReadVersionRequest req{DeviceID};
    if (!writeFrame(to_child_write_, Function::kPassThruReadVersion, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruReadVersionResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruReadVersion, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    // The vendor DLLs behind this bridge fill these as fixed-size,
    // null-terminated buffers per the J2534 spec's 80-byte version-string
    // convention; copy that whole fixed shape back to the caller's buffer.
    if (pApiVersion)
    {
        std::memcpy(pApiVersion, resp.api_version.data(), resp.api_version.size());
    }
    if (pDllVersion)
    {
        std::memcpy(pDllVersion, resp.dll_version.data(), resp.dll_version.size());
    }
    if (pFirmwareVersion)
    {
        std::memcpy(pFirmwareVersion, resp.firmware_version.data(), resp.firmware_version.size());
    }
    return resp.result;
}

long J2534BridgeClient::PassThruGetLastError(char *pErrorDescription)
{
    PassThruGetLastErrorRequest req{};
    if (!writeFrame(to_child_write_, Function::kPassThruGetLastError, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }
    PassThruGetLastErrorResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruGetLastError, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }
    if (pErrorDescription)
    {
        std::memcpy(pErrorDescription, resp.error_description.data(), resp.error_description.size());
    }
    return resp.result;
}

long J2534BridgeClient::PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput)
{
    PassThruIoctlRequest req{};
    req.channel_id = ChannelID;
    req.ioctl_id = IoctlID;

    // Mirror image of j2534_bridge_host/main.cpp's handlePassThruIoctl: that
    // function unpacks the typed Request fields back into the SConfigList/
    // SByteArray/unsigned-long* shapes PassThruIoctl's vendor signature
    // expects; here we pack the caller's pInput into those same Request
    // fields before sending. IoctlIDs the host doesn't give a typed shape to
    // are sent as a bare request (channel_id/ioctl_id only) and rejected by
    // the host's own default case with ERR_INVALID_IOCTL_ID -- no need to
    // duplicate that validation on the client side.
    switch (IoctlID)
    {
    case kJ2534SetConfig:
    {
        const auto *scl = static_cast<const SConfigList *>(pInput);
        constexpr unsigned long kMaxConfigParams = std::tuple_size_v<decltype(req.config_params)>;
        if (!scl || scl->num_of_params > kMaxConfigParams)
        {
            return kJ2534ErrFailed;
        }
        req.num_config_params = scl->num_of_params;
        for (unsigned long i = 0; i < scl->num_of_params; ++i)
        {
            req.config_params[i] = scl->config_ptr[i];
        }
        break;
    }
    case kJ2534FiveBaudInit:
    case kJ2534FastInit:
    {
        const auto *inArr = static_cast<const SByteArray *>(pInput);
        if (!inArr || inArr->num_of_bytes > req.input_bytes.size())
        {
            return kJ2534ErrFailed;
        }
        req.input_byte_count = inArr->num_of_bytes;
        std::memcpy(req.input_bytes.data(), inArr->byte_ptr, inArr->num_of_bytes);
        break;
    }
    case kJ2534ReadVbatt:
    case kJ2534ReadProgVoltage:
    case kJ2534ClearRxBuffer:
    case kJ2534ClearTxBuffer:
    case kJ2534ClearPeriodicMsgs:
    case kJ2534ClearMsgFilters:
    default:
        // NULL input (READ_VBATT/READ_PROG_VOLTAGE/CLEAR_*) or an
        // unrecognized id -- nothing to pack.
        break;
    }

    if (!writeFrame(to_child_write_, Function::kPassThruIoctl, &req, sizeof(req)))
    {
        stop();
        return kJ2534ErrFailed;
    }

    PassThruIoctlResponse resp{};
    if (!readAndValidateResponse(from_child_read_, Function::kPassThruIoctl, resp))
    {
        stop();
        return kJ2534ErrFailed;
    }

    switch (IoctlID)
    {
    case kJ2534FiveBaudInit:
    case kJ2534FastInit:
    {
        auto *outArr = static_cast<SByteArray *>(pOutput);
        if (outArr)
        {
            unsigned long n = resp.output_byte_count;
            if (n > outArr->num_of_bytes)
            {
                n = outArr->num_of_bytes; // never overflow the caller's buffer
            }
            std::memcpy(outArr->byte_ptr, resp.output_bytes.data(), n);
            outArr->num_of_bytes = n;
        }
        break;
    }
    case kJ2534ReadVbatt:
    case kJ2534ReadProgVoltage:
    {
        auto *vbatt = static_cast<unsigned long *>(pOutput);
        if (vbatt)
        {
            *vbatt = resp.vbatt;
        }
        break;
    }
    default:
        break;
    }

    return resp.result;
}
