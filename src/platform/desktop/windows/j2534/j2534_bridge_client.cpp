#include "src/platform/desktop/windows/j2534/j2534_bridge_client.h"
#include "src/platform/desktop/windows/j2534/j2534_bridge_protocol.h"

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
template <typename Resp>
bool readAndValidateResponse(HANDLE pipe, Function expectedFunction, Resp& resp)
{
    FrameHeader header{};
    if (!readFrame(pipe, header, &resp, sizeof(resp)))
        return false;
    return header.function == expectedFunction;
}

} // namespace

J2534BridgeClient::J2534BridgeClient(std::string hostExePath, std::string vendorDllPath)
    : hostExePath_(std::move(hostExePath)), vendorDllPath_(std::move(vendorDllPath))
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
        return false;
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

    std::string cmdLine = "\"" + hostExePath_ + "\" \"" + vendorDllPath_ + "\"";
    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cmdLineBuf.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                             &si, &processInfo_);
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
    jobObject_ = CreateJobObjectA(nullptr, nullptr);
    if (jobObject_)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(jobObject_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(jobObject_, processInfo_.hProcess);
    }
    ResumeThread(processInfo_.hThread);

    toChildWrite_ = childStdinWrite;
    fromChildRead_ = childStdoutRead;
    running_ = true;
    return true;
}

void J2534BridgeClient::stop()
{
    if (!running_)
        return;
    writeFrame(toChildWrite_, Function::Shutdown, nullptr, 0);
    WaitForSingleObject(processInfo_.hProcess, 2000);
    CloseHandle(toChildWrite_);
    CloseHandle(fromChildRead_);
    CloseHandle(processInfo_.hProcess);
    CloseHandle(processInfo_.hThread);
    if (jobObject_)
        CloseHandle(jobObject_);
    running_ = false;
}

long J2534BridgeClient::PassThruOpen(const void *pName, unsigned long *pDeviceID)
{
    PassThruOpenRequest req{};
    if (pName)
    {
        req.hasName = true;
        strncpy_s(req.name, static_cast<const char *>(pName), sizeof(req.name) - 1);
    }
    if (!writeFrame(toChildWrite_, Function::PassThruOpen, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruOpenResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruOpen, resp))
    {
        stop();
        return ERR_FAILED;
    }
    *pDeviceID = resp.deviceId;
    return resp.result;
}

long J2534BridgeClient::PassThruClose(unsigned long DeviceID)
{
    PassThruCloseRequest req{DeviceID};
    if (!writeFrame(toChildWrite_, Function::PassThruClose, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruCloseResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruClose, resp))
    {
        stop();
        return ERR_FAILED;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags,
                                        unsigned long Baudrate, unsigned long *pChannelID)
{
    PassThruConnectRequest req{DeviceID, ProtocolID, Flags, Baudrate};
    if (!writeFrame(toChildWrite_, Function::PassThruConnect, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruConnectResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruConnect, resp))
    {
        stop();
        return ERR_FAILED;
    }
    *pChannelID = resp.channelId;
    return resp.result;
}

long J2534BridgeClient::PassThruDisconnect(unsigned long ChannelID)
{
    PassThruDisconnectRequest req{ChannelID};
    if (!writeFrame(toChildWrite_, Function::PassThruDisconnect, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruDisconnectResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruDisconnect, resp))
    {
        stop();
        return ERR_FAILED;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs,
                                         unsigned long Timeout)
{
    PassThruReadMsgsRequest req{ChannelID, Timeout};
    if (!writeFrame(toChildWrite_, Function::PassThruReadMsgs, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruReadMsgsResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruReadMsgs, resp))
    {
        stop();
        return ERR_FAILED;
    }
    *pMsg = resp.msg;
    *pNumMsgs = resp.numMsgs;
    return resp.result;
}

long J2534BridgeClient::PassThruWriteMsgs(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs,
                                          unsigned long Timeout)
{
    PassThruWriteMsgsRequest req{};
    req.channelId = ChannelID;
    req.timeout = Timeout;
    if (pMsg)
        req.msg = *pMsg;
    if (!writeFrame(toChildWrite_, Function::PassThruWriteMsgs, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruWriteMsgsResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruWriteMsgs, resp))
    {
        stop();
        return ERR_FAILED;
    }
    if (pNumMsgs)
        *pNumMsgs = resp.numMsgs;
    return resp.result;
}

long J2534BridgeClient::PassThruStartPeriodicMsg(unsigned long ChannelID, const PASSTHRU_MSG *pMsg,
                                                 unsigned long *pMsgID, unsigned long TimeInterval)
{
    PassThruStartPeriodicMsgRequest req{};
    req.channelId = ChannelID;
    req.timeInterval = TimeInterval;
    if (pMsg)
        req.msg = *pMsg;
    if (!writeFrame(toChildWrite_, Function::PassThruStartPeriodicMsg, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruStartPeriodicMsgResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruStartPeriodicMsg, resp))
    {
        stop();
        return ERR_FAILED;
    }
    *pMsgID = resp.msgId;
    return resp.result;
}

long J2534BridgeClient::PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID)
{
    PassThruStopPeriodicMsgRequest req{ChannelID, MsgID};
    if (!writeFrame(toChildWrite_, Function::PassThruStopPeriodicMsg, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruStopPeriodicMsgResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruStopPeriodicMsg, resp))
    {
        stop();
        return ERR_FAILED;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType,
                                               const PASSTHRU_MSG *pMaskMsg, const PASSTHRU_MSG *pPatternMsg,
                                               const PASSTHRU_MSG *pFlowControlMsg, unsigned long *pMsgID)
{
    PassThruStartMsgFilterRequest req{};
    req.channelId = ChannelID;
    req.filterType = FilterType;
    if (pMaskMsg)
        req.maskMsg = *pMaskMsg;
    if (pPatternMsg)
        req.patternMsg = *pPatternMsg;
    req.hasFlowControlMsg = (pFlowControlMsg != nullptr);
    if (pFlowControlMsg)
        req.flowControlMsg = *pFlowControlMsg;
    if (!writeFrame(toChildWrite_, Function::PassThruStartMsgFilter, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruStartMsgFilterResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruStartMsgFilter, resp))
    {
        stop();
        return ERR_FAILED;
    }
    *pMsgID = resp.msgId;
    return resp.result;
}

long J2534BridgeClient::PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID)
{
    PassThruStopMsgFilterRequest req{ChannelID, MsgID};
    if (!writeFrame(toChildWrite_, Function::PassThruStopMsgFilter, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruStopMsgFilterResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruStopMsgFilter, resp))
    {
        stop();
        return ERR_FAILED;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin,
                                                      unsigned long Voltage)
{
    PassThruSetProgrammingVoltageRequest req{DeviceID, Pin, Voltage};
    if (!writeFrame(toChildWrite_, Function::PassThruSetProgrammingVoltage, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruSetProgrammingVoltageResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruSetProgrammingVoltage, resp))
    {
        stop();
        return ERR_FAILED;
    }
    return resp.result;
}

long J2534BridgeClient::PassThruReadVersion(char *pApiVersion, char *pDllVersion, char *pFirmwareVersion,
                                            unsigned long DeviceID)
{
    PassThruReadVersionRequest req{DeviceID};
    if (!writeFrame(toChildWrite_, Function::PassThruReadVersion, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruReadVersionResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruReadVersion, resp))
    {
        stop();
        return ERR_FAILED;
    }
    // The vendor DLLs behind this bridge fill these as fixed-size,
    // null-terminated buffers per the J2534 spec's 80-byte version-string
    // convention; copy that whole fixed shape back to the caller's buffer.
    if (pApiVersion)
        std::memcpy(pApiVersion, resp.apiVersion, sizeof(resp.apiVersion));
    if (pDllVersion)
        std::memcpy(pDllVersion, resp.dllVersion, sizeof(resp.dllVersion));
    if (pFirmwareVersion)
        std::memcpy(pFirmwareVersion, resp.firmwareVersion, sizeof(resp.firmwareVersion));
    return resp.result;
}

long J2534BridgeClient::PassThruGetLastError(char *pErrorDescription)
{
    PassThruGetLastErrorRequest req{};
    if (!writeFrame(toChildWrite_, Function::PassThruGetLastError, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }
    PassThruGetLastErrorResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruGetLastError, resp))
    {
        stop();
        return ERR_FAILED;
    }
    if (pErrorDescription)
        std::memcpy(pErrorDescription, resp.errorDescription, sizeof(resp.errorDescription));
    return resp.result;
}

long J2534BridgeClient::PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput,
                                      void *pOutput)
{
    PassThruIoctlRequest req{};
    req.channelId = ChannelID;
    req.ioctlId = IoctlID;

    // Mirror image of j2534_bridge_host/main.cpp's handlePassThruIoctl: that
    // function unpacks the typed Request fields back into the SCONFIG_LIST/
    // SBYTE_ARRAY/unsigned-long* shapes PassThruIoctl's vendor signature
    // expects; here we pack the caller's pInput into those same Request
    // fields before sending. IoctlIDs the host doesn't give a typed shape to
    // are sent as a bare request (channelId/ioctlId only) and rejected by
    // the host's own default case with ERR_INVALID_IOCTL_ID -- no need to
    // duplicate that validation on the client side.
    switch (IoctlID)
    {
    case SET_CONFIG:
    {
        const auto *scl = static_cast<const SCONFIG_LIST *>(pInput);
        constexpr unsigned long kMaxConfigParams = sizeof(req.configParams) / sizeof(req.configParams[0]);
        if (!scl || scl->NumOfParams > kMaxConfigParams)
            return ERR_FAILED;
        req.numConfigParams = scl->NumOfParams;
        for (unsigned long i = 0; i < scl->NumOfParams; ++i)
            req.configParams[i] = scl->ConfigPtr[i];
        break;
    }
    case FIVE_BAUD_INIT:
    case FAST_INIT:
    {
        const auto *inArr = static_cast<const SBYTE_ARRAY *>(pInput);
        if (!inArr || inArr->NumOfBytes > sizeof(req.inputBytes))
            return ERR_FAILED;
        req.inputByteCount = inArr->NumOfBytes;
        std::memcpy(req.inputBytes, inArr->BytePtr, inArr->NumOfBytes);
        break;
    }
    case READ_VBATT:
    case READ_PROG_VOLTAGE:
    case CLEAR_RX_BUFFER:
    case CLEAR_TX_BUFFER:
    case CLEAR_PERIODIC_MSGS:
    case CLEAR_MSG_FILTERS:
    default:
        // NULL input (READ_VBATT/READ_PROG_VOLTAGE/CLEAR_*) or an
        // unrecognized id -- nothing to pack.
        break;
    }

    if (!writeFrame(toChildWrite_, Function::PassThruIoctl, &req, sizeof(req)))
    {
        stop();
        return ERR_FAILED;
    }

    PassThruIoctlResponse resp{};
    if (!readAndValidateResponse(fromChildRead_, Function::PassThruIoctl, resp))
    {
        stop();
        return ERR_FAILED;
    }

    switch (IoctlID)
    {
    case FIVE_BAUD_INIT:
    case FAST_INIT:
    {
        auto *outArr = static_cast<SBYTE_ARRAY *>(pOutput);
        if (outArr)
        {
            unsigned long n = resp.outputByteCount;
            if (n > outArr->NumOfBytes)
                n = outArr->NumOfBytes; // never overflow the caller's buffer
            std::memcpy(outArr->BytePtr, resp.outputBytes, n);
            outArr->NumOfBytes = n;
        }
        break;
    }
    case READ_VBATT:
    case READ_PROG_VOLTAGE:
    {
        auto *vbatt = static_cast<unsigned long *>(pOutput);
        if (vbatt)
            *vbatt = resp.vbatt;
        break;
    }
    default:
        break;
    }

    return resp.result;
}
