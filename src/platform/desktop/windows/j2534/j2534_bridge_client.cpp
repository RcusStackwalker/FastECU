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
template <typename Resp> bool ReadAndValidateResponse(HANDLE pipe, Function expected_function, Resp& resp)
{
    FrameHeader header{};
    if (!ReadFrame(pipe, header, &resp, sizeof(resp)))
    {
        return false;
    }
    return header.function == expected_function;
}

} // namespace

J2534BridgeClient::J2534BridgeClient(std::string host_exe_path, std::string vendor_dll_path)
    : host_exe_path_(std::move(host_exe_path)), vendor_dll_path_(std::move(vendor_dll_path))
{
}

J2534BridgeClient::~J2534BridgeClient()
{
    Stop();
}

bool J2534BridgeClient::Start()
{
    HANDLE child_stdin_read, child_stdin_write, child_stdout_read, child_stdout_write;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0))
    {
        return false;
    }
    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0))
    {
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdin_write);
        return false;
    }
    SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_read;
    si.hStdOutput = child_stdout_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::string cmd_line = "\"" + host_exe_path_ + "\" \"" + vendor_dll_path_ + "\"";
    std::vector<char> cmd_line_buf(cmd_line.begin(), cmd_line.end());
    cmd_line_buf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cmd_line_buf.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                             &si, &process_info_);
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    if (!ok)
    {
        CloseHandle(child_stdin_write);
        CloseHandle(child_stdout_read);
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

    to_child_write_ = child_stdin_write;
    from_child_read_ = child_stdout_read;
    running_ = true;
    return true;
}

void J2534BridgeClient::Stop()
{
    if (!running_)
    {
        return;
    }
    WriteFrame(to_child_write_, Function::kShutdown, nullptr, 0);
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

long J2534BridgeClient::PassThruOpen(const void *p_name, unsigned long *p_device_id)
{
    PassThruOpenRequest req{};
    if (p_name)
    {
        req.has_name = true;
        strncpy_s(req.name.data(), req.name.size(), static_cast<const char *>(p_name), req.name.size() - 1);
    }
    if (!WriteFrame(to_child_write_, Function::kPassThruOpen, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruOpenResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruOpen, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    *p_device_id = resp.device_id;
    return resp.result;
}

long J2534BridgeClient::PassThruClose(unsigned long device_id)
{
    PassThruCloseRequest req{device_id};
    if (!WriteFrame(to_child_write_, Function::kPassThruClose, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruCloseResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruClose, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruConnect(unsigned long device_id, unsigned long protocol_id, unsigned long flags,
                                        unsigned long baudrate, unsigned long *p_channel_id)
{
    PassThruConnectRequest req{device_id, protocol_id, flags, baudrate};
    if (!WriteFrame(to_child_write_, Function::kPassThruConnect, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruConnectResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruConnect, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    *p_channel_id = resp.channel_id;
    return resp.result;
}

long J2534BridgeClient::PassThruDisconnect(unsigned long channel_id)
{
    PassThruDisconnectRequest req{channel_id};
    if (!WriteFrame(to_child_write_, Function::kPassThruDisconnect, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruDisconnectResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruDisconnect, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruReadMsgs(unsigned long channel_id, PassThruMsg *p_msg, unsigned long *p_num_msgs,
                                         unsigned long timeout)
{
    PassThruReadMsgsRequest req{channel_id, timeout};
    if (!WriteFrame(to_child_write_, Function::kPassThruReadMsgs, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruReadMsgsResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruReadMsgs, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    *p_msg = resp.msg;
    *p_num_msgs = resp.num_msgs;
    return resp.result;
}

long J2534BridgeClient::PassThruWriteMsgs(unsigned long channel_id, const PassThruMsg *p_msg, unsigned long *p_num_msgs,
                                          unsigned long timeout)
{
    PassThruWriteMsgsRequest req{};
    req.channel_id = channel_id;
    req.timeout = timeout;
    if (p_msg)
    {
        req.msg = *p_msg;
    }
    if (!WriteFrame(to_child_write_, Function::kPassThruWriteMsgs, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruWriteMsgsResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruWriteMsgs, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    if (p_num_msgs)
    {
        *p_num_msgs = resp.num_msgs;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruStartPeriodicMsg(unsigned long channel_id, const PassThruMsg *p_msg,
                                                 unsigned long *p_msg_id, unsigned long time_interval)
{
    PassThruStartPeriodicMsgRequest req{};
    req.channel_id = channel_id;
    req.time_interval = time_interval;
    if (p_msg)
    {
        req.msg = *p_msg;
    }
    if (!WriteFrame(to_child_write_, Function::kPassThruStartPeriodicMsg, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruStartPeriodicMsgResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruStartPeriodicMsg, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    *p_msg_id = resp.msg_id;
    return resp.result;
}

long J2534BridgeClient::PassThruStopPeriodicMsg(unsigned long channel_id, unsigned long msg_id)
{
    PassThruStopPeriodicMsgRequest req{channel_id, msg_id};
    if (!WriteFrame(to_child_write_, Function::kPassThruStopPeriodicMsg, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruStopPeriodicMsgResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruStopPeriodicMsg, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruStartMsgFilter(unsigned long channel_id, unsigned long filter_type,
                                               const PassThruMsg *p_mask_msg, const PassThruMsg *p_pattern_msg,
                                               const PassThruMsg *p_flow_control_msg, unsigned long *p_msg_id)
{
    PassThruStartMsgFilterRequest req{};
    req.channel_id = channel_id;
    req.filter_type = filter_type;
    if (p_mask_msg)
    {
        req.mask_msg = *p_mask_msg;
    }
    if (p_pattern_msg)
    {
        req.pattern_msg = *p_pattern_msg;
    }
    req.has_flow_control_msg = (p_flow_control_msg != nullptr);
    if (p_flow_control_msg)
    {
        req.flow_control_msg = *p_flow_control_msg;
    }
    if (!WriteFrame(to_child_write_, Function::kPassThruStartMsgFilter, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruStartMsgFilterResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruStartMsgFilter, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    *p_msg_id = resp.msg_id;
    return resp.result;
}

long J2534BridgeClient::PassThruStopMsgFilter(unsigned long channel_id, unsigned long msg_id)
{
    PassThruStopMsgFilterRequest req{channel_id, msg_id};
    if (!WriteFrame(to_child_write_, Function::kPassThruStopMsgFilter, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruStopMsgFilterResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruStopMsgFilter, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruSetProgrammingVoltage(unsigned long device_id, unsigned long pin, unsigned long voltage)
{
    PassThruSetProgrammingVoltageRequest req{device_id, pin, voltage};
    if (!WriteFrame(to_child_write_, Function::kPassThruSetProgrammingVoltage, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruSetProgrammingVoltageResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruSetProgrammingVoltage, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruReadVersion(char *p_api_version, char *p_dll_version, char *p_firmware_version,
                                            unsigned long device_id)
{
    PassThruReadVersionRequest req{device_id};
    if (!WriteFrame(to_child_write_, Function::kPassThruReadVersion, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruReadVersionResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruReadVersion, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    // The vendor DLLs behind this bridge fill these as fixed-size,
    // null-terminated buffers per the J2534 spec's 80-byte version-string
    // convention; copy that whole fixed shape back to the caller's buffer.
    if (p_api_version)
    {
        std::memcpy(p_api_version, resp.api_version.data(), resp.api_version.size());
    }
    if (p_dll_version)
    {
        std::memcpy(p_dll_version, resp.dll_version.data(), resp.dll_version.size());
    }
    if (p_firmware_version)
    {
        std::memcpy(p_firmware_version, resp.firmware_version.data(), resp.firmware_version.size());
    }
    return resp.result;
}

long J2534BridgeClient::PassThruGetLastError(char *p_error_description)
{
    PassThruGetLastErrorRequest req{};
    if (!WriteFrame(to_child_write_, Function::kPassThruGetLastError, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    PassThruGetLastErrorResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruGetLastError, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }
    if (p_error_description)
    {
        std::memcpy(p_error_description, resp.error_description.data(), resp.error_description.size());
    }
    return resp.result;
}

long J2534BridgeClient::PassThruIoctl(unsigned long channel_id, unsigned long ioctl_id, const void *p_input,
                                      void *p_output)
{
    PassThruIoctlRequest req{};
    req.channel_id = channel_id;
    req.ioctl_id = ioctl_id;

    // Mirror image of j2534_bridge_host/main.cpp's handlePassThruIoctl: that
    // function unpacks the typed Request fields back into the SConfigList/
    // SByteArray/unsigned-long* shapes PassThruIoctl's vendor signature
    // expects; here we pack the caller's pInput into those same Request
    // fields before sending. IoctlIDs the host doesn't give a typed shape to
    // are sent as a bare request (channel_id/ioctl_id only) and rejected by
    // the host's own default case with ERR_INVALID_IOCTL_ID -- no need to
    // duplicate that validation on the client side.
    switch (ioctl_id)
    {
    case kJ2534SetConfig:
    {
        const auto *scl = static_cast<const SConfigList *>(p_input);
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
        const auto *in_arr = static_cast<const SByteArray *>(p_input);
        if (!in_arr || in_arr->num_of_bytes > req.input_bytes.size())
        {
            return kJ2534ErrFailed;
        }
        req.input_byte_count = in_arr->num_of_bytes;
        std::memcpy(req.input_bytes.data(), in_arr->byte_ptr, in_arr->num_of_bytes);
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

    if (!WriteFrame(to_child_write_, Function::kPassThruIoctl, &req, sizeof(req)))
    {
        Stop();
        return kJ2534ErrFailed;
    }

    PassThruIoctlResponse resp{};
    if (!ReadAndValidateResponse(from_child_read_, Function::kPassThruIoctl, resp))
    {
        Stop();
        return kJ2534ErrFailed;
    }

    switch (ioctl_id)
    {
    case kJ2534FiveBaudInit:
    case kJ2534FastInit:
    {
        auto *out_arr = static_cast<SByteArray *>(p_output);
        if (out_arr)
        {
            unsigned long n = resp.output_byte_count;
            if (n > out_arr->num_of_bytes)
            {
                n = out_arr->num_of_bytes; // never overflow the caller's buffer
            }
            std::memcpy(out_arr->byte_ptr, resp.output_bytes.data(), n);
            out_arr->num_of_bytes = n;
        }
        break;
    }
    case kJ2534ReadVbatt:
    case kJ2534ReadProgVoltage:
    {
        auto *vbatt = static_cast<unsigned long *>(p_output);
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
