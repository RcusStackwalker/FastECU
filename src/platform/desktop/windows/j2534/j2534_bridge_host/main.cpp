#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

using namespace j2534_bridge;

namespace
{

using PF_PassThruOpen = long(PT_CALL *)(const void *, unsigned long *);
using PF_PassThruClose = long(PT_CALL *)(unsigned long);
using PF_PassThruConnect = long(PT_CALL *)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long *);
using PF_PassThruDisconnect = long(PT_CALL *)(unsigned long);
using PF_PassThruReadMsgs = long(PT_CALL *)(unsigned long, PASSTHRU_MSG *, unsigned long *, unsigned long);
using PF_PassThruWriteMsgs = long(PT_CALL *)(unsigned long, const PASSTHRU_MSG *, unsigned long *, unsigned long);
using PF_PassThruStartPeriodicMsg = long(PT_CALL *)(unsigned long, const PASSTHRU_MSG *, unsigned long *, unsigned long);
using PF_PassThruStopPeriodicMsg = long(PT_CALL *)(unsigned long, unsigned long);
using PF_PassThruStartMsgFilter = long(PT_CALL *)(unsigned long, unsigned long, const PASSTHRU_MSG *,
                                                  const PASSTHRU_MSG *, const PASSTHRU_MSG *, unsigned long *);
using PF_PassThruStopMsgFilter = long(PT_CALL *)(unsigned long, unsigned long);
using PF_PassThruSetProgrammingVoltage = long(PT_CALL *)(unsigned long, unsigned long, unsigned long);
using PF_PassThruReadVersion = long(PT_CALL *)(unsigned long, char *, char *, char *);
using PF_PassThruGetLastError = long(PT_CALL *)(char *);
using PF_PassThruIoctl = long(PT_CALL *)(unsigned long, unsigned long, const void *, void *);

struct VendorApi
{
    HMODULE module = nullptr;
    PF_PassThruOpen open = nullptr;
    PF_PassThruClose close = nullptr;
    PF_PassThruConnect connect = nullptr;
    PF_PassThruDisconnect disconnect = nullptr;
    PF_PassThruReadMsgs readMsgs = nullptr;
    PF_PassThruWriteMsgs writeMsgs = nullptr;
    PF_PassThruStartPeriodicMsg startPeriodicMsg = nullptr;
    PF_PassThruStopPeriodicMsg stopPeriodicMsg = nullptr;
    PF_PassThruStartMsgFilter startMsgFilter = nullptr;
    PF_PassThruStopMsgFilter stopMsgFilter = nullptr;
    PF_PassThruSetProgrammingVoltage setProgrammingVoltage = nullptr;
    PF_PassThruReadVersion readVersion = nullptr;
    PF_PassThruGetLastError getLastError = nullptr;
    PF_PassThruIoctl ioctl = nullptr;
};

bool loadVendorApi(const char *dllPath, VendorApi& api)
{
    api.module = LoadLibraryA(dllPath);
    if (!api.module)
        return false;
    // GetProcAddress names must match the DLL's exported symbol names exactly
    // (see tests/fake_j2534_dll.def for the fixture's matching export list).
    api.open = reinterpret_cast<PF_PassThruOpen>(GetProcAddress(api.module, "PassThruOpen"));
    api.close = reinterpret_cast<PF_PassThruClose>(GetProcAddress(api.module, "PassThruClose"));
    api.connect = reinterpret_cast<PF_PassThruConnect>(GetProcAddress(api.module, "PassThruConnect"));
    api.disconnect = reinterpret_cast<PF_PassThruDisconnect>(GetProcAddress(api.module, "PassThruDisconnect"));
    api.readMsgs = reinterpret_cast<PF_PassThruReadMsgs>(GetProcAddress(api.module, "PassThruReadMsgs"));
    api.writeMsgs = reinterpret_cast<PF_PassThruWriteMsgs>(GetProcAddress(api.module, "PassThruWriteMsgs"));
    api.startPeriodicMsg =
        reinterpret_cast<PF_PassThruStartPeriodicMsg>(GetProcAddress(api.module, "PassThruStartPeriodicMsg"));
    api.stopPeriodicMsg =
        reinterpret_cast<PF_PassThruStopPeriodicMsg>(GetProcAddress(api.module, "PassThruStopPeriodicMsg"));
    api.startMsgFilter =
        reinterpret_cast<PF_PassThruStartMsgFilter>(GetProcAddress(api.module, "PassThruStartMsgFilter"));
    api.stopMsgFilter = reinterpret_cast<PF_PassThruStopMsgFilter>(GetProcAddress(api.module, "PassThruStopMsgFilter"));
    api.setProgrammingVoltage = reinterpret_cast<PF_PassThruSetProgrammingVoltage>(
        GetProcAddress(api.module, "PassThruSetProgrammingVoltage"));
    api.readVersion = reinterpret_cast<PF_PassThruReadVersion>(GetProcAddress(api.module, "PassThruReadVersion"));
    api.getLastError = reinterpret_cast<PF_PassThruGetLastError>(GetProcAddress(api.module, "PassThruGetLastError"));
    api.ioctl = reinterpret_cast<PF_PassThruIoctl>(GetProcAddress(api.module, "PassThruIoctl"));

    return api.open && api.close && api.connect && api.disconnect && api.readMsgs && api.writeMsgs &&
           api.startPeriodicMsg && api.stopPeriodicMsg && api.startMsgFilter && api.stopMsgFilter &&
           api.setProgrammingVoltage && api.readVersion && api.getLastError && api.ioctl;
}

// Every dispatch function has the same shape: read the fixed-size payload
// (the header has already been consumed by runLoop), call the resolved
// vendor function, encode the fixed-size Response, send it. Two shapes
// recur: a plain-argument call (PassThruOpen/Close/Connect/Disconnect/
// StartPeriodicMsg/StopPeriodicMsg/StartMsgFilter/StopMsgFilter/
// SetProgrammingVoltage/ReadVersion/GetLastError), a call taking one
// PASSTHRU_MSG (ReadMsgs/WriteMsgs), and PassThruIoctl's ID-based
// sub-dispatch, which is its own shape.

// Reads and discards `size` bytes from the pipe without interpreting them --
// used to resync the stream after a payload-size mismatch, so the next
// readFrameHeader() call sees a real header instead of leftover payload
// bytes from the frame that didn't match.
bool drainPayload(HANDLE pipe, std::uint32_t size)
{
    if (size == 0)
        return true;
    std::vector<char> discard(size);
    return readFramePayload(pipe, discard.data(), size);
}

// Reads a fixed-size Req payload following an already-read FrameHeader. On a
// size mismatch, drains the actual (differently-sized) payload to keep the
// stream framed, then sends an ERR_FAILED Resp so a blocking caller doesn't
// hang, and returns false. On a genuine I/O failure (broken pipe), no resync
// or response is possible -- just returns false. On success, req is
// populated and this returns true.
template <typename Req, typename Resp>
bool readTypedRequest(HANDLE in, HANDLE out, const FrameHeader& header, Function respondAs, Req& req)
{
    if (header.payloadSize != sizeof(req))
    {
        drainPayload(in, header.payloadSize);
        Resp errResp{};
        errResp.result = ERR_FAILED;
        writeFrame(out, respondAs, &errResp, sizeof(errResp));
        return false;
    }
    if (!readFramePayload(in, &req, sizeof(req)))
        return false;
    return true;
}

void handlePassThruOpen(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruOpenRequest req{};
    if (!readTypedRequest<PassThruOpenRequest, PassThruOpenResponse>(in, out, header, Function::PassThruOpen, req))
        return;
    PassThruOpenResponse resp{};
    unsigned long deviceId = 0;
    resp.result = api.open(req.hasName ? req.name : nullptr, &deviceId);
    resp.deviceId = deviceId;
    writeFrame(out, Function::PassThruOpen, &resp, sizeof(resp));
}

void handlePassThruClose(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruCloseRequest req{};
    if (!readTypedRequest<PassThruCloseRequest, PassThruCloseResponse>(in, out, header, Function::PassThruClose, req))
        return;
    PassThruCloseResponse resp{};
    resp.result = api.close(req.deviceId);
    writeFrame(out, Function::PassThruClose, &resp, sizeof(resp));
}

void handlePassThruConnect(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruConnectRequest req{};
    if (!readTypedRequest<PassThruConnectRequest, PassThruConnectResponse>(in, out, header, Function::PassThruConnect, req))
        return;
    PassThruConnectResponse resp{};
    unsigned long channelId = 0;
    resp.result = api.connect(req.deviceId, req.protocolId, req.flags, req.baudrate, &channelId);
    resp.channelId = channelId;
    writeFrame(out, Function::PassThruConnect, &resp, sizeof(resp));
}

void handlePassThruDisconnect(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruDisconnectRequest req{};
    if (!readTypedRequest<PassThruDisconnectRequest, PassThruDisconnectResponse>(in, out, header, Function::PassThruDisconnect, req))
        return;
    PassThruDisconnectResponse resp{};
    resp.result = api.disconnect(req.channelId);
    writeFrame(out, Function::PassThruDisconnect, &resp, sizeof(resp));
}

void handlePassThruReadMsgs(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruReadMsgsRequest req{};
    if (!readTypedRequest<PassThruReadMsgsRequest, PassThruReadMsgsResponse>(in, out, header, Function::PassThruReadMsgs, req))
        return;
    PassThruReadMsgsResponse resp{};
    unsigned long numMsgs = 1;
    resp.result = api.readMsgs(req.channelId, &resp.msg, &numMsgs, req.timeout);
    resp.numMsgs = numMsgs;
    writeFrame(out, Function::PassThruReadMsgs, &resp, sizeof(resp));
}

void handlePassThruWriteMsgs(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruWriteMsgsRequest req{};
    if (!readTypedRequest<PassThruWriteMsgsRequest, PassThruWriteMsgsResponse>(in, out, header, Function::PassThruWriteMsgs, req))
        return;
    PassThruWriteMsgsResponse resp{};
    unsigned long numMsgs = 1;
    resp.result = api.writeMsgs(req.channelId, &req.msg, &numMsgs, req.timeout);
    resp.numMsgs = numMsgs;
    writeFrame(out, Function::PassThruWriteMsgs, &resp, sizeof(resp));
}

void handlePassThruStartPeriodicMsg(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruStartPeriodicMsgRequest req{};
    if (!readTypedRequest<PassThruStartPeriodicMsgRequest, PassThruStartPeriodicMsgResponse>(in, out, header, Function::PassThruStartPeriodicMsg, req))
        return;
    PassThruStartPeriodicMsgResponse resp{};
    unsigned long msgId = 0;
    resp.result = api.startPeriodicMsg(req.channelId, &req.msg, &msgId, req.timeInterval);
    resp.msgId = msgId;
    writeFrame(out, Function::PassThruStartPeriodicMsg, &resp, sizeof(resp));
}

void handlePassThruStopPeriodicMsg(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruStopPeriodicMsgRequest req{};
    if (!readTypedRequest<PassThruStopPeriodicMsgRequest, PassThruStopPeriodicMsgResponse>(in, out, header, Function::PassThruStopPeriodicMsg, req))
        return;
    PassThruStopPeriodicMsgResponse resp{};
    resp.result = api.stopPeriodicMsg(req.channelId, req.msgId);
    writeFrame(out, Function::PassThruStopPeriodicMsg, &resp, sizeof(resp));
}

void handlePassThruStartMsgFilter(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruStartMsgFilterRequest req{};
    if (!readTypedRequest<PassThruStartMsgFilterRequest, PassThruStartMsgFilterResponse>(in, out, header, Function::PassThruStartMsgFilter, req))
        return;
    PassThruStartMsgFilterResponse resp{};
    unsigned long msgId = 0;
    resp.result = api.startMsgFilter(req.channelId, req.filterType, &req.maskMsg, &req.patternMsg,
                                     req.hasFlowControlMsg ? &req.flowControlMsg : nullptr, &msgId);
    resp.msgId = msgId;
    writeFrame(out, Function::PassThruStartMsgFilter, &resp, sizeof(resp));
}

void handlePassThruStopMsgFilter(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruStopMsgFilterRequest req{};
    if (!readTypedRequest<PassThruStopMsgFilterRequest, PassThruStopMsgFilterResponse>(in, out, header, Function::PassThruStopMsgFilter, req))
        return;
    PassThruStopMsgFilterResponse resp{};
    resp.result = api.stopMsgFilter(req.channelId, req.msgId);
    writeFrame(out, Function::PassThruStopMsgFilter, &resp, sizeof(resp));
}

void handlePassThruSetProgrammingVoltage(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruSetProgrammingVoltageRequest req{};
    if (!readTypedRequest<PassThruSetProgrammingVoltageRequest, PassThruSetProgrammingVoltageResponse>(in, out, header, Function::PassThruSetProgrammingVoltage, req))
        return;
    PassThruSetProgrammingVoltageResponse resp{};
    resp.result = api.setProgrammingVoltage(req.deviceId, req.pin, req.voltage);
    writeFrame(out, Function::PassThruSetProgrammingVoltage, &resp, sizeof(resp));
}

void handlePassThruReadVersion(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruReadVersionRequest req{};
    if (!readTypedRequest<PassThruReadVersionRequest, PassThruReadVersionResponse>(in, out, header, Function::PassThruReadVersion, req))
        return;
    PassThruReadVersionResponse resp{};
    resp.result = api.readVersion(req.deviceId, resp.apiVersion, resp.dllVersion, resp.firmwareVersion);
    writeFrame(out, Function::PassThruReadVersion, &resp, sizeof(resp));
}

void handlePassThruGetLastError(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruGetLastErrorRequest req{};
    if (!readTypedRequest<PassThruGetLastErrorRequest, PassThruGetLastErrorResponse>(in, out, header, Function::PassThruGetLastError, req))
        return;
    PassThruGetLastErrorResponse resp{};
    resp.result = api.getLastError(resp.errorDescription);
    writeFrame(out, Function::PassThruGetLastError, &resp, sizeof(resp));
}

void handlePassThruIoctl(const VendorApi& api, HANDLE in, HANDLE out, const FrameHeader& header)
{
    PassThruIoctlRequest req{};
    if (!readTypedRequest<PassThruIoctlRequest, PassThruIoctlResponse>(in, out, header, Function::PassThruIoctl, req))
        return;
    PassThruIoctlResponse resp{};

    switch (req.ioctlId)
    {
    case SET_CONFIG:
    {
        SCONFIG_LIST scl{req.numConfigParams, req.configParams};
        resp.result = api.ioctl(req.channelId, req.ioctlId, &scl, nullptr);
        break;
    }
    case FIVE_BAUD_INIT:
    case FAST_INIT:
    {
        SBYTE_ARRAY inArr{req.inputByteCount, req.inputBytes};
        SBYTE_ARRAY outArr{sizeof(resp.outputBytes), resp.outputBytes};
        resp.result = api.ioctl(req.channelId, req.ioctlId, &inArr, &outArr);
        resp.outputByteCount = outArr.NumOfBytes;
        break;
    }
    case READ_VBATT:
    case READ_PROG_VOLTAGE:
    {
        unsigned long vbatt = 0;
        resp.result = api.ioctl(req.channelId, req.ioctlId, nullptr, &vbatt);
        resp.vbatt = vbatt;
        break;
    }
    case CLEAR_RX_BUFFER:
    case CLEAR_TX_BUFFER:
    case CLEAR_PERIODIC_MSGS:
    case CLEAR_MSG_FILTERS:
        resp.result = api.ioctl(req.channelId, req.ioctlId, nullptr, nullptr);
        break;
    default:
        resp.result = ERR_INVALID_IOCTL_ID;
        break;
    }

    writeFrame(out, Function::PassThruIoctl, &resp, sizeof(resp));
}

// runLoop() reads the fixed-size FrameHeader first via readFrameHeader (the
// header alone is enough to know the Function tag; the payload's shape isn't
// known until then), then dispatches on it. Each handle*() reads its own
// payload via readFramePayload once its Request type is known.
bool runLoop(const VendorApi& api)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    for (;;)
    {
        FrameHeader header{};
        if (!readFrameHeader(in, header))
            return true; // pipe closed (parent exited) -- exit cleanly, not an error

        switch (header.function)
        {
        case Function::PassThruOpen:
            handlePassThruOpen(api, in, out, header);
            break;
        case Function::PassThruClose:
            handlePassThruClose(api, in, out, header);
            break;
        case Function::PassThruConnect:
            handlePassThruConnect(api, in, out, header);
            break;
        case Function::PassThruDisconnect:
            handlePassThruDisconnect(api, in, out, header);
            break;
        case Function::PassThruReadMsgs:
            handlePassThruReadMsgs(api, in, out, header);
            break;
        case Function::PassThruWriteMsgs:
            handlePassThruWriteMsgs(api, in, out, header);
            break;
        case Function::PassThruStartPeriodicMsg:
            handlePassThruStartPeriodicMsg(api, in, out, header);
            break;
        case Function::PassThruStopPeriodicMsg:
            handlePassThruStopPeriodicMsg(api, in, out, header);
            break;
        case Function::PassThruStartMsgFilter:
            handlePassThruStartMsgFilter(api, in, out, header);
            break;
        case Function::PassThruStopMsgFilter:
            handlePassThruStopMsgFilter(api, in, out, header);
            break;
        case Function::PassThruSetProgrammingVoltage:
            handlePassThruSetProgrammingVoltage(api, in, out, header);
            break;
        case Function::PassThruReadVersion:
            handlePassThruReadVersion(api, in, out, header);
            break;
        case Function::PassThruGetLastError:
            handlePassThruGetLastError(api, in, out, header);
            break;
        case Function::PassThruIoctl:
            handlePassThruIoctl(api, in, out, header);
            break;
        case Function::Shutdown:
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
    if (!loadVendorApi(argv[1], api))
    {
        PassThruOpenResponse errorResp{};
        errorResp.result = ERR_DEVICE_NOT_CONNECTED;
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        writeFrame(out, Function::PassThruOpen, &errorResp, sizeof(errorResp));
        return 1;
    }

    return runLoop(api) ? 0 : 1;
}
