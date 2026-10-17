#include "src/platform/desktop/windows/j2534/j2534_bridge_client.h"
#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

#include <algorithm>
#include <array>
#include <cstring>
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

bool PackConfig(const SConfigList *scl, PassThruIoctlRequest& req)
{
    if (!scl || scl->num_of_params > req.config_params.size())
    {
        return false;
    }
    req.num_config_params = scl->num_of_params;
    std::copy_n(scl->config_ptr, scl->num_of_params, req.config_params.begin());
    return true;
}

bool PackBytes(const SByteArray *in_arr, PassThruIoctlRequest& req)
{
    if (!in_arr || in_arr->num_of_bytes > req.input_bytes.size())
    {
        return false;
    }
    req.input_byte_count = in_arr->num_of_bytes;
    std::memcpy(req.input_bytes.data(), in_arr->byte_ptr, in_arr->num_of_bytes);
    return true;
}

// Mirror image of j2534_bridge_host/main.cpp's CallIoctl: that function
// unpacks the typed Request fields back into the SConfigList/SByteArray/
// unsigned-long* shapes PassThruIoctl's vendor signature expects; here we
// pack the caller's pInput into those same Request fields before sending.
// IoctlIDs the host doesn't give a typed shape to are sent as a bare request
// (channel_id/ioctl_id only) and rejected by the host's own default case with
// ERR_INVALID_IOCTL_ID -- no need to duplicate that validation on the client
// side. Returns false when the input does not fit the request.
bool PackIoctlInput(const void *p_input, PassThruIoctlRequest& req)
{
    switch (req.ioctl_id)
    {
    case kJ2534SetConfig:
        return PackConfig(static_cast<const SConfigList *>(p_input), req);
    case kJ2534FiveBaudInit:
    case kJ2534FastInit:
        return PackBytes(static_cast<const SByteArray *>(p_input), req);
    default:
        // NULL input (READ_VBATT/READ_PROG_VOLTAGE/CLEAR_*) or an
        // unrecognized id -- nothing to pack.
        return true;
    }
}

void UnpackIoctlOutput(unsigned long ioctl_id, const PassThruIoctlResponse& resp, void *p_output)
{
    if (!p_output)
    {
        return;
    }
    switch (ioctl_id)
    {
    case kJ2534FiveBaudInit:
    case kJ2534FastInit:
    {
        auto& out_arr = *static_cast<SByteArray *>(p_output);
        // never overflow the caller's buffer
        out_arr.num_of_bytes = std::min(resp.output_byte_count, out_arr.num_of_bytes);
        std::memcpy(out_arr.byte_ptr, resp.output_bytes.data(), out_arr.num_of_bytes);
        break;
    }
    case kJ2534ReadVbatt:
    case kJ2534ReadProgVoltage:
        *static_cast<unsigned long *>(p_output) = resp.vbatt;
        break;
    default:
        break;
    }
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

template <typename Req, typename Resp> bool J2534BridgeClient::Call(Function function, const Req& req, Resp& resp)
{
    if (WriteFrame(to_child_write_, function, &req, sizeof(req)) &&
        ReadAndValidateResponse(from_child_read_, function, resp))
    {
        return true;
    }
    Stop();
    return false;
}

long J2534BridgeClient::PassThruOpen(const void *p_name, unsigned long *p_device_id)
{
    PassThruOpenRequest req{};
    if (p_name)
    {
        req.has_name = true;
        strncpy_s(req.name.data(), req.name.size(), static_cast<const char *>(p_name), req.name.size() - 1);
    }
    PassThruOpenResponse resp{};
    if (!Call(Function::kPassThruOpen, req, resp))
    {
        return kJ2534ErrFailed;
    }
    *p_device_id = resp.device_id;
    return resp.result;
}

long J2534BridgeClient::PassThruClose(unsigned long device_id)
{
    PassThruCloseRequest req{device_id};
    PassThruCloseResponse resp{};
    return Call(Function::kPassThruClose, req, resp) ? resp.result : kJ2534ErrFailed;
}

long J2534BridgeClient::PassThruConnect(unsigned long device_id, unsigned long protocol_id, unsigned long flags,
                                        unsigned long baudrate, unsigned long *p_channel_id)
{
    PassThruConnectRequest req{device_id, protocol_id, flags, baudrate};
    PassThruConnectResponse resp{};
    if (!Call(Function::kPassThruConnect, req, resp))
    {
        return kJ2534ErrFailed;
    }
    *p_channel_id = resp.channel_id;
    return resp.result;
}

long J2534BridgeClient::PassThruDisconnect(unsigned long channel_id)
{
    PassThruDisconnectRequest req{channel_id};
    PassThruDisconnectResponse resp{};
    return Call(Function::kPassThruDisconnect, req, resp) ? resp.result : kJ2534ErrFailed;
}

long J2534BridgeClient::PassThruReadMsgs(unsigned long channel_id, PassThruMsg *p_msg, unsigned long *p_num_msgs,
                                         unsigned long timeout)
{
    PassThruReadMsgsRequest req{channel_id, timeout};
    PassThruReadMsgsResponse resp{};
    if (!Call(Function::kPassThruReadMsgs, req, resp))
    {
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
    PassThruWriteMsgsResponse resp{};
    if (!Call(Function::kPassThruWriteMsgs, req, resp))
    {
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
    PassThruStartPeriodicMsgResponse resp{};
    if (!Call(Function::kPassThruStartPeriodicMsg, req, resp))
    {
        return kJ2534ErrFailed;
    }
    *p_msg_id = resp.msg_id;
    return resp.result;
}

long J2534BridgeClient::PassThruStopPeriodicMsg(unsigned long channel_id, unsigned long msg_id)
{
    PassThruStopPeriodicMsgRequest req{channel_id, msg_id};
    PassThruStopPeriodicMsgResponse resp{};
    return Call(Function::kPassThruStopPeriodicMsg, req, resp) ? resp.result : kJ2534ErrFailed;
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
    PassThruStartMsgFilterResponse resp{};
    if (!Call(Function::kPassThruStartMsgFilter, req, resp))
    {
        return kJ2534ErrFailed;
    }
    *p_msg_id = resp.msg_id;
    return resp.result;
}

long J2534BridgeClient::PassThruStopMsgFilter(unsigned long channel_id, unsigned long msg_id)
{
    PassThruStopMsgFilterRequest req{channel_id, msg_id};
    PassThruStopMsgFilterResponse resp{};
    return Call(Function::kPassThruStopMsgFilter, req, resp) ? resp.result : kJ2534ErrFailed;
}

long J2534BridgeClient::PassThruSetProgrammingVoltage(unsigned long device_id, unsigned long pin, unsigned long voltage)
{
    PassThruSetProgrammingVoltageRequest req{device_id, pin, voltage};
    PassThruSetProgrammingVoltageResponse resp{};
    return Call(Function::kPassThruSetProgrammingVoltage, req, resp) ? resp.result : kJ2534ErrFailed;
}

long J2534BridgeClient::PassThruReadVersion(char *p_api_version, char *p_dll_version, char *p_firmware_version,
                                            unsigned long device_id)
{
    PassThruReadVersionRequest req{device_id};
    PassThruReadVersionResponse resp{};
    if (!Call(Function::kPassThruReadVersion, req, resp))
    {
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
    PassThruGetLastErrorResponse resp{};
    if (!Call(Function::kPassThruGetLastError, req, resp))
    {
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
    if (!PackIoctlInput(p_input, req))
    {
        return kJ2534ErrFailed;
    }
    PassThruIoctlResponse resp{};
    if (!Call(Function::kPassThruIoctl, req, resp))
    {
        return kJ2534ErrFailed;
    }
    UnpackIoctlOutput(ioctl_id, resp, p_output);
    return resp.result;
}
