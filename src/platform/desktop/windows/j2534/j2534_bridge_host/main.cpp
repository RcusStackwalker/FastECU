#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

using namespace j2534_bridge;

namespace
{

using PfPassThruOpen = long(PT_CALL *)(const void *, unsigned long *);
using PfPassThruClose = long(PT_CALL *)(unsigned long);
using PfPassThruConnect = long(PT_CALL *)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long *);
using PfPassThruDisconnect = long(PT_CALL *)(unsigned long);
using PfPassThruReadMsgs = long(PT_CALL *)(unsigned long, PassThruMsg *, unsigned long *, unsigned long);
using PfPassThruWriteMsgs = long(PT_CALL *)(unsigned long, const PassThruMsg *, unsigned long *, unsigned long);
using PfPassThruStartPeriodicMsg = long(PT_CALL *)(unsigned long, const PassThruMsg *, unsigned long *, unsigned long);
using PfPassThruStopPeriodicMsg = long(PT_CALL *)(unsigned long, unsigned long);
using PfPassThruStartMsgFilter = long(PT_CALL *)(unsigned long, unsigned long, const PassThruMsg *, const PassThruMsg *,
                                                 const PassThruMsg *, unsigned long *);
using PfPassThruStopMsgFilter = long(PT_CALL *)(unsigned long, unsigned long);
using PfPassThruSetProgrammingVoltage = long(PT_CALL *)(unsigned long, unsigned long, unsigned long);
using PfPassThruReadVersion = long(PT_CALL *)(unsigned long, char *, char *, char *);
using PfPassThruGetLastError = long(PT_CALL *)(char *);
using PfPassThruIoctl = long(PT_CALL *)(unsigned long, unsigned long, const void *, void *);

struct VendorApi
{
    HMODULE module = nullptr;
    PfPassThruOpen open = nullptr;
    PfPassThruClose close = nullptr;
    PfPassThruConnect connect = nullptr;
    PfPassThruDisconnect disconnect = nullptr;
    PfPassThruReadMsgs read_msgs = nullptr;
    PfPassThruWriteMsgs write_msgs = nullptr;
    PfPassThruStartPeriodicMsg start_periodic_msg = nullptr;
    PfPassThruStopPeriodicMsg stop_periodic_msg = nullptr;
    PfPassThruStartMsgFilter start_msg_filter = nullptr;
    PfPassThruStopMsgFilter stop_msg_filter = nullptr;
    PfPassThruSetProgrammingVoltage set_programming_voltage = nullptr;
    PfPassThruReadVersion read_version = nullptr;
    PfPassThruGetLastError get_last_error = nullptr;
    PfPassThruIoctl ioctl = nullptr;
};

bool LoadVendorApi(const char *dll_path, VendorApi& api)
{
    api.module = LoadLibraryA(dll_path);
    if (!api.module)
    {
        return false;
    }
    // GetProcAddress names must match the DLL's exported symbol names exactly
    // (see tests/fake_j2534_dll.def for the fixture's matching export list).
    api.open = reinterpret_cast<PfPassThruOpen>(GetProcAddress(api.module, "PassThruOpen"));
    api.close = reinterpret_cast<PfPassThruClose>(GetProcAddress(api.module, "PassThruClose"));
    api.connect = reinterpret_cast<PfPassThruConnect>(GetProcAddress(api.module, "PassThruConnect"));
    api.disconnect = reinterpret_cast<PfPassThruDisconnect>(GetProcAddress(api.module, "PassThruDisconnect"));
    api.read_msgs = reinterpret_cast<PfPassThruReadMsgs>(GetProcAddress(api.module, "PassThruReadMsgs"));
    api.write_msgs = reinterpret_cast<PfPassThruWriteMsgs>(GetProcAddress(api.module, "PassThruWriteMsgs"));
    api.start_periodic_msg =
        reinterpret_cast<PfPassThruStartPeriodicMsg>(GetProcAddress(api.module, "PassThruStartPeriodicMsg"));
    api.stop_periodic_msg =
        reinterpret_cast<PfPassThruStopPeriodicMsg>(GetProcAddress(api.module, "PassThruStopPeriodicMsg"));
    api.start_msg_filter =
        reinterpret_cast<PfPassThruStartMsgFilter>(GetProcAddress(api.module, "PassThruStartMsgFilter"));
    api.stop_msg_filter =
        reinterpret_cast<PfPassThruStopMsgFilter>(GetProcAddress(api.module, "PassThruStopMsgFilter"));
    api.set_programming_voltage =
        reinterpret_cast<PfPassThruSetProgrammingVoltage>(GetProcAddress(api.module, "PassThruSetProgrammingVoltage"));
    api.read_version = reinterpret_cast<PfPassThruReadVersion>(GetProcAddress(api.module, "PassThruReadVersion"));
    api.get_last_error = reinterpret_cast<PfPassThruGetLastError>(GetProcAddress(api.module, "PassThruGetLastError"));
    api.ioctl = reinterpret_cast<PfPassThruIoctl>(GetProcAddress(api.module, "PassThruIoctl"));

    return api.open && api.close && api.connect && api.disconnect && api.read_msgs && api.write_msgs &&
           api.start_periodic_msg && api.stop_periodic_msg && api.start_msg_filter && api.stop_msg_filter &&
           api.set_programming_voltage && api.read_version && api.get_last_error && api.ioctl;
}

// Every dispatch function has the same shape: read the fixed-size payload
// (the header has already been consumed by runLoop), call the resolved
// vendor function, encode the fixed-size Response, send it. Two shapes
// recur: a plain-argument call (PassThruOpen/Close/Connect/Disconnect/
// StartPeriodicMsg/StopPeriodicMsg/StartMsgFilter/StopMsgFilter/
// SetProgrammingVoltage/ReadVersion/GetLastError), a call taking one
// PassThruMsg (ReadMsgs/WriteMsgs), and PassThruIoctl's ID-based
// sub-dispatch, which is its own shape.

// Reads and discards `size` bytes from the pipe without interpreting them --
// used to resync the stream after a payload-size mismatch, so the next
// readFrameHeader() call sees a real header instead of leftover payload
// bytes from the frame that didn't match.
bool DrainPayload(HANDLE pipe, std::uint32_t size)
{
    if (size == 0)
    {
        return true;
    }
    std::vector<char> discard(size);
    return ReadFramePayload(pipe, discard.data(), size);
}

// Reads a fixed-size Req payload following an already-read FrameHeader. On a
// size mismatch, drains the actual (differently-sized) payload to keep the
// stream framed, then sends an ERR_FAILED Resp so a blocking caller doesn't
// hang, and returns false. On a genuine I/O failure (broken pipe), no resync
// or response is possible -- just returns false. On success, req is
// populated and this returns true.
template <typename Req, typename Resp>
bool ReadTypedRequest(HANDLE in, HANDLE out, const FrameHeader& header, Function respond_as, Req& req)
{
    if (header.payload_size != sizeof(req))
    {
        DrainPayload(in, header.payload_size);
        Resp err_resp{};
        err_resp.result = kJ2534ErrFailed;
        WriteFrame(out, respond_as, &err_resp, sizeof(err_resp));
        return false;
    }
    if (!ReadFramePayload(in, &req, sizeof(req)))
    {
        return false;
    }
    return true;
}

void HandlePassThruOpen(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruOpenRequest req{};
    if (!ReadTypedRequest<PassThruOpenRequest, PassThruOpenResponse>(in, out, header, Function::kPassThruOpen, req))
    {
        return;
    }
    PassThruOpenResponse resp{};
    unsigned long device_id = 0;
    resp.result = api.open(req.has_name ? req.name.data() : nullptr, &device_id);
    resp.device_id = device_id;
    WriteFrame(out, Function::kPassThruOpen, &resp, sizeof(resp));
}

void HandlePassThruClose(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruCloseRequest req{};
    if (!ReadTypedRequest<PassThruCloseRequest, PassThruCloseResponse>(in, out, header, Function::kPassThruClose, req))
    {
        return;
    }
    PassThruCloseResponse resp{};
    resp.result = api.close(req.device_id);
    WriteFrame(out, Function::kPassThruClose, &resp, sizeof(resp));
}

void HandlePassThruConnect(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruConnectRequest req{};
    if (!ReadTypedRequest<PassThruConnectRequest, PassThruConnectResponse>(in, out, header, Function::kPassThruConnect,
                                                                           req))
    {
        return;
    }
    PassThruConnectResponse resp{};
    unsigned long channel_id = 0;
    resp.result = api.connect(req.device_id, req.protocol_id, req.flags, req.baudrate, &channel_id);
    resp.channel_id = channel_id;
    WriteFrame(out, Function::kPassThruConnect, &resp, sizeof(resp));
}

void HandlePassThruDisconnect(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruDisconnectRequest req{};
    if (!ReadTypedRequest<PassThruDisconnectRequest, PassThruDisconnectResponse>(in, out, header,
                                                                                 Function::kPassThruDisconnect, req))
    {
        return;
    }
    PassThruDisconnectResponse resp{};
    resp.result = api.disconnect(req.channel_id);
    WriteFrame(out, Function::kPassThruDisconnect, &resp, sizeof(resp));
}

void HandlePassThruReadMsgs(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruReadMsgsRequest req{};
    if (!ReadTypedRequest<PassThruReadMsgsRequest, PassThruReadMsgsResponse>(in, out, header,
                                                                             Function::kPassThruReadMsgs, req))
    {
        return;
    }
    PassThruReadMsgsResponse resp{};
    unsigned long num_msgs = 1;
    resp.result = api.read_msgs(req.channel_id, &resp.msg, &num_msgs, req.timeout);
    resp.num_msgs = num_msgs;
    WriteFrame(out, Function::kPassThruReadMsgs, &resp, sizeof(resp));
}

void HandlePassThruWriteMsgs(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruWriteMsgsRequest req{};
    if (!ReadTypedRequest<PassThruWriteMsgsRequest, PassThruWriteMsgsResponse>(in, out, header,
                                                                               Function::kPassThruWriteMsgs, req))
    {
        return;
    }
    PassThruWriteMsgsResponse resp{};
    unsigned long num_msgs = 1;
    resp.result = api.write_msgs(req.channel_id, &req.msg, &num_msgs, req.timeout);
    resp.num_msgs = num_msgs;
    WriteFrame(out, Function::kPassThruWriteMsgs, &resp, sizeof(resp));
}

void HandlePassThruStartPeriodicMsg(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruStartPeriodicMsgRequest req{};
    if (!ReadTypedRequest<PassThruStartPeriodicMsgRequest, PassThruStartPeriodicMsgResponse>(
            in, out, header, Function::kPassThruStartPeriodicMsg, req))
    {
        return;
    }
    PassThruStartPeriodicMsgResponse resp{};
    unsigned long msg_id = 0;
    resp.result = api.start_periodic_msg(req.channel_id, &req.msg, &msg_id, req.time_interval);
    resp.msg_id = msg_id;
    WriteFrame(out, Function::kPassThruStartPeriodicMsg, &resp, sizeof(resp));
}

void HandlePassThruStopPeriodicMsg(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruStopPeriodicMsgRequest req{};
    if (!ReadTypedRequest<PassThruStopPeriodicMsgRequest, PassThruStopPeriodicMsgResponse>(
            in, out, header, Function::kPassThruStopPeriodicMsg, req))
    {
        return;
    }
    PassThruStopPeriodicMsgResponse resp{};
    resp.result = api.stop_periodic_msg(req.channel_id, req.msg_id);
    WriteFrame(out, Function::kPassThruStopPeriodicMsg, &resp, sizeof(resp));
}

void HandlePassThruStartMsgFilter(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruStartMsgFilterRequest req{};
    if (!ReadTypedRequest<PassThruStartMsgFilterRequest, PassThruStartMsgFilterResponse>(
            in, out, header, Function::kPassThruStartMsgFilter, req))
    {
        return;
    }
    PassThruStartMsgFilterResponse resp{};
    unsigned long msg_id = 0;
    resp.result = api.start_msg_filter(req.channel_id, req.filter_type, &req.mask_msg, &req.pattern_msg,
                                       req.has_flow_control_msg ? &req.flow_control_msg : nullptr, &msg_id);
    resp.msg_id = msg_id;
    WriteFrame(out, Function::kPassThruStartMsgFilter, &resp, sizeof(resp));
}

void HandlePassThruStopMsgFilter(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruStopMsgFilterRequest req{};
    if (!ReadTypedRequest<PassThruStopMsgFilterRequest, PassThruStopMsgFilterResponse>(
            in, out, header, Function::kPassThruStopMsgFilter, req))
    {
        return;
    }
    PassThruStopMsgFilterResponse resp{};
    resp.result = api.stop_msg_filter(req.channel_id, req.msg_id);
    WriteFrame(out, Function::kPassThruStopMsgFilter, &resp, sizeof(resp));
}

void HandlePassThruSetProgrammingVoltage(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruSetProgrammingVoltageRequest req{};
    if (!ReadTypedRequest<PassThruSetProgrammingVoltageRequest, PassThruSetProgrammingVoltageResponse>(
            in, out, header, Function::kPassThruSetProgrammingVoltage, req))
    {
        return;
    }
    PassThruSetProgrammingVoltageResponse resp{};
    resp.result = api.set_programming_voltage(req.device_id, req.pin, req.voltage);
    WriteFrame(out, Function::kPassThruSetProgrammingVoltage, &resp, sizeof(resp));
}

void HandlePassThruReadVersion(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruReadVersionRequest req{};
    if (!ReadTypedRequest<PassThruReadVersionRequest, PassThruReadVersionResponse>(in, out, header,
                                                                                   Function::kPassThruReadVersion, req))
    {
        return;
    }
    PassThruReadVersionResponse resp{};
    resp.result =
        api.read_version(req.device_id, resp.api_version.data(), resp.dll_version.data(), resp.firmware_version.data());
    WriteFrame(out, Function::kPassThruReadVersion, &resp, sizeof(resp));
}

void HandlePassThruGetLastError(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruGetLastErrorRequest req{};
    if (!ReadTypedRequest<PassThruGetLastErrorRequest, PassThruGetLastErrorResponse>(
            in, out, header, Function::kPassThruGetLastError, req))
    {
        return;
    }
    PassThruGetLastErrorResponse resp{};
    resp.result = api.get_last_error(resp.error_description.data());
    WriteFrame(out, Function::kPassThruGetLastError, &resp, sizeof(resp));
}

void HandlePassThruIoctl(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruIoctlRequest req{};
    if (!ReadTypedRequest<PassThruIoctlRequest, PassThruIoctlResponse>(in, out, header, Function::kPassThruIoctl, req))
    {
        return;
    }
    PassThruIoctlResponse resp{};

    switch (req.ioctl_id)
    {
    case kJ2534SetConfig:
    {
        SConfigList scl{req.num_config_params, req.config_params.data()};
        resp.result = api.ioctl(req.channel_id, req.ioctl_id, &scl, nullptr);
        break;
    }
    case kJ2534FiveBaudInit:
    case kJ2534FastInit:
    {
        SByteArray in_arr{req.input_byte_count, req.input_bytes.data()};
        SByteArray out_arr{static_cast<unsigned long>(resp.output_bytes.size()), resp.output_bytes.data()};
        resp.result = api.ioctl(req.channel_id, req.ioctl_id, &in_arr, &out_arr);
        resp.output_byte_count = out_arr.num_of_bytes;
        break;
    }
    case kJ2534ReadVbatt:
    case kJ2534ReadProgVoltage:
    {
        unsigned long vbatt = 0;
        resp.result = api.ioctl(req.channel_id, req.ioctl_id, nullptr, &vbatt);
        resp.vbatt = vbatt;
        break;
    }
    case kJ2534ClearRxBuffer:
    case kJ2534ClearTxBuffer:
    case kJ2534ClearPeriodicMsgs:
    case kJ2534ClearMsgFilters:
        resp.result = api.ioctl(req.channel_id, req.ioctl_id, nullptr, nullptr);
        break;
    default:
        resp.result = kJ2534ErrInvalidIoctlId;
        break;
    }

    WriteFrame(out, Function::kPassThruIoctl, &resp, sizeof(resp));
}

// runLoop() reads the fixed-size FrameHeader first via readFrameHeader (the
// header alone is enough to know the Function tag; the payload's shape isn't
// known until then), then dispatches on it. Each handle*() reads its own
// payload via readFramePayload once its Request type is known.
bool RunLoop(const VendorApi& api)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    for (;;)
    {
        FrameHeader header{};
        if (!ReadFrameHeader(in, header))
        {
            return true; // pipe closed (parent exited) -- exit cleanly, not an error
        }

        switch (header.function)
        {
        case Function::kPassThruOpen:
            HandlePassThruOpen(api, in, out, header);
            break;
        case Function::kPassThruClose:
            HandlePassThruClose(api, in, out, header);
            break;
        case Function::kPassThruConnect:
            HandlePassThruConnect(api, in, out, header);
            break;
        case Function::kPassThruDisconnect:
            HandlePassThruDisconnect(api, in, out, header);
            break;
        case Function::kPassThruReadMsgs:
            HandlePassThruReadMsgs(api, in, out, header);
            break;
        case Function::kPassThruWriteMsgs:
            HandlePassThruWriteMsgs(api, in, out, header);
            break;
        case Function::kPassThruStartPeriodicMsg:
            HandlePassThruStartPeriodicMsg(api, in, out, header);
            break;
        case Function::kPassThruStopPeriodicMsg:
            HandlePassThruStopPeriodicMsg(api, in, out, header);
            break;
        case Function::kPassThruStartMsgFilter:
            HandlePassThruStartMsgFilter(api, in, out, header);
            break;
        case Function::kPassThruStopMsgFilter:
            HandlePassThruStopMsgFilter(api, in, out, header);
            break;
        case Function::kPassThruSetProgrammingVoltage:
            HandlePassThruSetProgrammingVoltage(api, in, out, header);
            break;
        case Function::kPassThruReadVersion:
            HandlePassThruReadVersion(api, in, out, header);
            break;
        case Function::kPassThruGetLastError:
            HandlePassThruGetLastError(api, in, out, header);
            break;
        case Function::kPassThruIoctl:
            HandlePassThruIoctl(api, in, out, header);
            break;
        case Function::kShutdown:
            return true;
        default:
            break;
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: j2534_bridge_host <vendor-dll-path>\n");
        return 1;
    }

    VendorApi api;
    if (!LoadVendorApi(argv[1], api))
    {
        PassThruOpenResponse error_resp{};
        error_resp.result = kJ2534ErrDeviceNotConnected;
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        WriteFrame(out, Function::kPassThruOpen, &error_resp, sizeof(error_resp));
        return 1;
    }

    return RunLoop(api) ? 0 : 1;
}
