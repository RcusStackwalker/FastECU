#include "apps/bench/bench_commands.h"

#include <algorithm>
#include <cstdint>
#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/uds/uds_response.h"

namespace fastecu::bench
{
namespace
{

constexpr uds::ExchangePolicy kRoutinePolicy{.read_timeout_ms = 500};
constexpr uds::ExchangePolicy kSlowPolicy{.read_timeout_ms = 3000};
constexpr std::uint64_t kMaxWireU24 = 0xFFFFFF;

Status validateWireRange(std::uint32_t address, std::uint64_t length, std::string_view subject)
{
    if (length == 0)
    {
        return fail(ErrorKind::InvalidConfig, std::format("{} length must not be zero", subject));
    }
    if (address > kMaxWireU24)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} address 0x{:x} does not fit the 24-bit wire field", subject, address));
    }
    if (length > kMaxWireU24)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} length {} does not fit the 24-bit wire field", subject, length));
    }
    const std::uint64_t last = static_cast<std::uint64_t>(address) + length - 1;
    if (last > kMaxWireU24)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} range 0x{:x}..0x{:x} exceeds the 24-bit address space", subject, address, last));
    }
    return {};
}

bool isKnownDestructivePdu(bytes::ByteView pdu)
{
    if (pdu.empty())
    {
        return false;
    }
    if (pdu[0] == MitsuColtCan::kServiceRequestReflash || pdu[0] == MitsuColtCan::kServiceRequestDownload ||
        pdu[0] == MitsuColtCan::kServiceTransferData)
    {
        return true;
    }
    return pdu.size() >= 2 && pdu[0] == MitsuColtCan::kServiceRoutineControl && pdu[1] == MitsuColtCan::kRoutineErase;
}

void mergeTraffic(CommandOutcome& outcome, const TrafficEvidence& traffic)
{
    if (traffic.exchange_count == 0)
    {
        return;
    }
    if (outcome.exchange_count == 0)
    {
        outcome.tx = traffic.tx;
        outcome.rx = traffic.rx;
    }
    outcome.last_tx = traffic.last_tx;
    outcome.last_rx = traffic.last_rx;
    outcome.exchange_count += traffic.exchange_count;
    outcome.elapsed_ms += traffic.elapsed_ms;
}

Result<bytes::Bytes> exchange(BenchContext& context, CommandOutcome& outcome, bytes::ByteView pdu,
                              const uds::ExchangePolicy& policy)
{
    Result<bytes::Bytes> result = context.session.exchange(pdu, policy);
    mergeTraffic(outcome, context.session.last_traffic());
    return result;
}

Result<bytes::Bytes> exchangeRaw(BenchContext& context, CommandOutcome& outcome, bytes::ByteView pdu, int timeout_ms)
{
    Result<bytes::Bytes> result = context.session.exchange_raw(pdu, timeout_ms);
    mergeTraffic(outcome, context.session.last_traffic());
    return result;
}

Status connect(BenchContext& context, CommandOutcome& outcome)
{
    Status result = context.session.connect();
    mergeTraffic(outcome, context.session.last_traffic());
    return result;
}

// Command name plus its arguments, e.g. "read 0x200 1" -- what format_text's
// first line and format_json's "step" field show the operator.
std::string renderStep(const StepSpec& step)
{
    std::string text;
    for (const CommandSpec& spec : command_table())
    {
        if (spec.id == step.id)
        {
            text = std::string(spec.name);
            break;
        }
    }
    for (const std::string& arg : step.args)
    {
        text += ' ';
        text += arg;
    }
    return text;
}

// Shared by Read and Dump: chunks [addr, addr+len) at
// MitsuColtCan::kFlashReadBlockSize, filling outcome.data/note and traffic. A reply
// shorter than the requested chunk is rejected rather than padded, since a
// silently truncated read would look like a shorter-than-requested memory
// region instead of the protocol error it is.
Status readIntoOutcome(BenchContext& context, const StepSpec& step, CommandOutcome& outcome)
{
    const Result<std::uint32_t> addr = parse_u32(step.args[0]);
    if (!addr.has_value())
    {
        return std::unexpected(addr.error());
    }
    const Result<std::uint32_t> len = parse_u32(step.args[1]);
    if (!len.has_value())
    {
        return std::unexpected(len.error());
    }
    if (const Status valid = validateWireRange(*addr, *len, "read"); !valid.has_value())
    {
        return valid;
    }

    std::uint32_t offset = 0;
    int chunk_count = 0;
    while (offset < *len)
    {
        const std::uint32_t remaining = *len - offset;
        const auto chunk_len =
            static_cast<bytes::Byte>(std::min<std::uint32_t>(remaining, MitsuColtCan::kFlashReadBlockSize));
        const bytes::Bytes pdu = MitsuColtCan::buildReadMemoryByAddress(*addr + offset, chunk_len);
        const Result<bytes::Bytes> reply = exchange(context, outcome, pdu, kRoutinePolicy);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        const bytes::ByteView payload = uds::payload(*reply);
        if (payload.size() < chunk_len)
        {
            return fail(ErrorKind::BadResponse,
                        std::format("short reply: expected {} bytes, got {}", chunk_len, payload.size()));
        }
        outcome.data.insert(outcome.data.end(), payload.begin(), payload.begin() + chunk_len);

        offset += chunk_len;
        ++chunk_count;
    }
    outcome.note = std::format("{} chunks", chunk_count);
    return {};
}

// The RAM-resident helper array plus its slot address for one named routine.
struct RoutineSlot
{
    std::string_view name;
    bytes::ByteView bytes;
    std::uint32_t ram_address;
};

Result<RoutineSlot> routine_slot(std::string_view name)
{
    using namespace MitsuColtCan;
    if (name == "erase-page")
    {
        return RoutineSlot{name, kErasePageRoutine, kEraseRoutineRamAddr};
    }
    if (name == "erase-redirect")
    {
        return RoutineSlot{name, kEraseRedirectRoutine, kEraseRoutineRamAddr};
    }
    if (name == "write-page")
    {
        return RoutineSlot{name, kWritePageRoutine, kWriteRoutineRamAddr};
    }
    if (name == "write-redirect")
    {
        return RoutineSlot{name, kWriteRedirectRoutine, kWriteRoutineRamAddr};
    }
    return fail(ErrorKind::InvalidConfig, std::format("unknown routine: {}", name));
}

// Shared by Download and UploadRoutine: RequestDownload, every TransferData
// frame, a second RequestDownload/TransferData pair carrying the running
// checksum at kCrcTransferAddress, then a RoutineControl 225 CRC check on
// `addr`. RequestDownload and TransferData match the desktop executor's 500ms
// policy; only the final CRC check uses the 3000ms slow policy.
Status upload(BenchContext& context, CommandOutcome& outcome, std::uint32_t addr, bytes::ByteView payload)
{
    if (const Status valid = validateWireRange(addr, payload.size(), "download"); !valid.has_value())
    {
        return valid;
    }
    const bytes::Bytes requestDownload =
        MitsuColtCan::buildRequestDownload(addr, static_cast<std::uint32_t>(payload.size()));
    if (const Result<bytes::Bytes> reply = exchange(context, outcome, requestDownload, kRoutinePolicy);
        !reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    for (const bytes::Bytes& frame : MitsuColtCan::buildTransferDataFrames(payload))
    {
        if (const Result<bytes::Bytes> reply = exchange(context, outcome, frame, kRoutinePolicy); !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
    }

    const bytes::Bytes crcRequestDownload =
        MitsuColtCan::buildRequestDownload(MitsuColtCan::kCrcTransferAddress, MitsuColtCan::kCrcTransferSize);
    if (const Result<bytes::Bytes> reply = exchange(context, outcome, crcRequestDownload, kRoutinePolicy);
        !reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::Bytes checksumBytes = bytes::composeBe(MitsuColtCan::checksum(payload));
    for (const bytes::Bytes& frame : MitsuColtCan::buildTransferDataFrames(checksumBytes))
    {
        if (const Result<bytes::Bytes> reply = exchange(context, outcome, frame, kRoutinePolicy); !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
    }

    const bytes::Bytes crcCheck = MitsuColtCan::buildRoutineCheckCrc(addr);
    const Result<bytes::Bytes> crcReply = exchange(context, outcome, crcCheck, kSlowPolicy);
    if (!crcReply.has_value())
    {
        return std::unexpected(crcReply.error());
    }
    const bytes::ByteView crcPayload = uds::payload(*crcReply);
    if (crcPayload.size() < 2 || crcPayload[0] != MitsuColtCan::kRoutineCheckCrc || crcPayload[1] != 0)
    {
        return fail(ErrorKind::BadResponse, decode_crc_reply(crcPayload));
    }
    return {};
}

} // namespace

std::string decode_erase_reply(bytes::ByteView payload)
{
    if (payload.size() < 2)
    {
        return "no status byte in reply";
    }
    if (payload[0] != MitsuColtCan::kRoutineErase)
    {
        return std::format("wrong routine echo: expected 0x{:02x}, got 0x{:02x}", MitsuColtCan::kRoutineErase,
                           payload[0]);
    }
    if (payload[1] == 0)
    {
        return "status=0x00 (erase reported success)";
    }
    // colt_commented.S writes cobd_data[2] = 1 at 0x5a28, reachable from the
    // pre-erase gate at 0x59b0 (!(fp58_f16 & 0x40) && !flash200_u8 -- the
    // erase is never attempted) and from the post-erase branch at 0x5a14
    // (flasher_try_erase_range_call returned 3). The reply carries nothing
    // that separates them, so the note says so rather than guessing.
    return std::format("status=0x{:02x} -> colt_commented.S 0x5a28, reachable from the pre-erase gate "
                       "(0x59b0) or erase-routine failure (0x5a14); ambiguous",
                       payload[1]);
}

std::string decode_crc_reply(bytes::ByteView payload)
{
    if (payload.size() < 2)
    {
        return "no status byte in reply";
    }
    if (payload[0] != MitsuColtCan::kRoutineCheckCrc)
    {
        return std::format("wrong routine echo: expected 0x{:02x}, got 0x{:02x}", MitsuColtCan::kRoutineCheckCrc,
                           payload[0]);
    }
    return payload[1] == 0 ? "status=0x00 (CRC matched)" : std::format("status=0x{:02x} (CRC mismatch)", payload[1]);
}

Result<PreparedStep> prepare_step(IBenchFiles& files, const StepSpec& step)
{
    PreparedStep prepared{.spec = step};
    const CommandSpec *spec = nullptr;
    for (const CommandSpec& candidate : command_table())
    {
        if (candidate.id == step.id)
        {
            spec = &candidate;
            break;
        }
    }
    if (spec == nullptr)
    {
        return fail(ErrorKind::Internal, "step has no command spec");
    }
    if (step.args.size() < spec->min_args || (spec->max_args != kUnbounded && step.args.size() > spec->max_args))
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} takes {}..{} arguments, got {}", spec->name, spec->min_args,
                                spec->max_args == kUnbounded ? std::string("*") : std::to_string(spec->max_args),
                                step.args.size()));
    }
    if (spec->destructive && !step.destructive_ack)
    {
        return fail(ErrorKind::InvalidConfig, std::format("{} needs --destructive", spec->name));
    }

    switch (step.id)
    {
    case CommandId::Read:
    case CommandId::Dump:
    {
        const Result<std::uint32_t> address = parse_u32(step.args[0]);
        if (!address.has_value())
        {
            return std::unexpected(address.error());
        }
        const Result<std::uint32_t> length = parse_u32(step.args[1]);
        if (!length.has_value())
        {
            return std::unexpected(length.error());
        }
        const Status valid = validateWireRange(*address, *length, "read");
        if (!valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        return prepared;
    }
    case CommandId::CrcCheck:
    {
        const Result<std::uint32_t> address = parse_u32(step.args[0]);
        if (!address.has_value())
        {
            return std::unexpected(address.error());
        }
        if (*address > kMaxWireU24)
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("CRC address 0x{:x} does not fit the 24-bit address space", *address));
        }
        return prepared;
    }
    case CommandId::Send:
    case CommandId::SendRaw:
    {
        const Result<bytes::Bytes> pdu = parse_hex_bytes(step.args);
        if (!pdu.has_value())
        {
            return std::unexpected(pdu.error());
        }
        if (isKnownDestructivePdu(*pdu))
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("{} cannot bypass a named destructive command", spec->name));
        }
        return prepared;
    }
    case CommandId::Download:
    {
        const Result<std::uint32_t> address = parse_u32(step.args[0]);
        if (!address.has_value())
        {
            return std::unexpected(address.error());
        }
        Result<bytes::Bytes> data = files.load(step.args[1]);
        if (!data.has_value())
        {
            return std::unexpected(data.error());
        }
        const Status valid = validateWireRange(*address, data->size(), "download");
        if (!valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        prepared.upload_payload = std::move(*data);
        return prepared;
    }
    case CommandId::UploadRoutine:
    {
        const Result<RoutineSlot> slot = routine_slot(step.args[0]);
        if (!slot.has_value())
        {
            return std::unexpected(slot.error());
        }
        bytes::Bytes payload(slot->bytes.begin(), slot->bytes.end());
        if (step.args.size() > 1)
        {
            if (step.args.size() != 3 || step.args[1] != "--from")
            {
                return fail(ErrorKind::InvalidConfig,
                            std::format("{}'s extra arguments must be --from <path>", spec->name));
            }
            Result<bytes::Bytes> loaded = files.load(step.args[2]);
            if (!loaded.has_value())
            {
                return std::unexpected(loaded.error());
            }
            payload = std::move(*loaded);
        }
        const Status valid = validateWireRange(slot->ram_address, payload.size(), "download");
        if (!valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        prepared.upload_payload = std::move(payload);
        return prepared;
    }
    case CommandId::Ports:
    case CommandId::Connect:
    case CommandId::Unlock:
    case CommandId::Erase:
        return prepared;
    }
    return fail(ErrorKind::Internal, "unhandled command during validation");
}

Status executeStep(BenchContext& context, const PreparedStep& prepared, CommandOutcome& outcome)
{
    const StepSpec& step = prepared.spec;
    const CommandSpec *spec = nullptr;
    for (const CommandSpec& candidate : command_table())
    {
        if (candidate.id == step.id)
        {
            spec = &candidate;
            break;
        }
    }
    if (spec == nullptr)
    {
        return fail(ErrorKind::Internal, "step has no command spec");
    }
    // bench_args gates this at parse time; repeated here so a StepSpec built
    // another way cannot reach the wire ungated.
    if (spec->destructive && !step.destructive_ack)
    {
        return fail(ErrorKind::InvalidConfig, std::format("{} needs --destructive", spec->name));
    }

    switch (step.id)
    {
    case CommandId::Read:
    {
        const Status result = readIntoOutcome(context, step, outcome);
        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }
        break;
    }
    case CommandId::Dump:
    {
        const Status result = readIntoOutcome(context, step, outcome);
        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }
        const Status saved = context.files.save(step.args[2], outcome.data);
        if (!saved.has_value())
        {
            return std::unexpected(saved.error());
        }
        break;
    }
    case CommandId::CrcCheck:
    {
        const Result<std::uint32_t> addr = parse_u32(step.args[0]);
        if (!addr.has_value())
        {
            return std::unexpected(addr.error());
        }
        if (*addr > kMaxWireU24)
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("CRC address 0x{:x} does not fit the 24-bit address space", *addr));
        }
        const bytes::Bytes pdu = MitsuColtCan::buildRoutineCheckCrc(*addr);
        const Result<bytes::Bytes> reply = exchange(context, outcome, pdu, kSlowPolicy);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        const bytes::ByteView payload = uds::payload(*reply);
        outcome.note = decode_crc_reply(payload);
        if (payload.size() < 2 || payload[0] != MitsuColtCan::kRoutineCheckCrc || payload[1] != 0)
        {
            return fail(ErrorKind::BadResponse, outcome.note);
        }
        break;
    }
    case CommandId::Send:
    {
        const Result<bytes::Bytes> pdu = parse_hex_bytes(step.args);
        if (!pdu.has_value())
        {
            return std::unexpected(pdu.error());
        }
        if (isKnownDestructivePdu(*pdu))
        {
            return fail(ErrorKind::InvalidConfig, "send cannot bypass a named destructive command");
        }
        const Result<bytes::Bytes> reply = exchange(context, outcome, *pdu, kRoutinePolicy);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        break;
    }
    case CommandId::SendRaw:
    {
        const Result<bytes::Bytes> pdu = parse_hex_bytes(step.args);
        if (!pdu.has_value())
        {
            return std::unexpected(pdu.error());
        }
        if (isKnownDestructivePdu(*pdu))
        {
            return fail(ErrorKind::InvalidConfig, "send-raw cannot bypass a named destructive command");
        }
        // exchange_raw bypasses SID/NRC validation entirely -- whatever comes
        // back is the observation the operator asked for, not something to
        // classify as success or failure by content. A genuine transport
        // error (nothing arrived at all) still propagates like every other
        // command's Result does.
        const Result<bytes::Bytes> reply = exchangeRaw(context, outcome, *pdu, context.options.timeout_ms);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        break;
    }
    case CommandId::Connect:
    {
        const Status connected = connect(context, outcome);
        if (!connected.has_value())
        {
            return std::unexpected(connected.error());
        }
        break;
    }
    case CommandId::Ports:
        // main.cpp (Task 7) handles `ports` before any session exists.
        return fail(ErrorKind::Unsupported, "ports does not use a session");
    case CommandId::Unlock:
    {
        const bytes::Bytes pdu = MitsuColtCan::buildRequestReflashUnlock();
        const Result<bytes::Bytes> reply = exchange(context, outcome, pdu, kSlowPolicy);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        break;
    }
    case CommandId::Erase:
    {
        const bytes::Bytes pdu = MitsuColtCan::buildRoutineErase();
        const Result<bytes::Bytes> reply = exchange(context, outcome, pdu, kSlowPolicy);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        const bytes::ByteView payload = uds::payload(*reply);
        outcome.note = decode_erase_reply(payload);
        if (payload.size() < 2 || payload[0] != MitsuColtCan::kRoutineErase || payload[1] != 0)
        {
            return fail(ErrorKind::BadResponse, outcome.note);
        }
        break;
    }
    case CommandId::Download:
    {
        const Result<std::uint32_t> addr = parse_u32(step.args[0]);
        if (!addr.has_value())
        {
            return std::unexpected(addr.error());
        }
        if (!prepared.upload_payload.has_value())
        {
            return fail(ErrorKind::Internal, "prepared download has no payload");
        }
        const Status uploaded = upload(context, outcome, *addr, *prepared.upload_payload);
        if (!uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
        outcome.note = std::format("uploaded {} bytes to 0x{:06x}", prepared.upload_payload->size(), *addr);
        break;
    }
    case CommandId::UploadRoutine:
    {
        const Result<RoutineSlot> slot = routine_slot(step.args[0]);
        if (!slot.has_value())
        {
            return std::unexpected(slot.error());
        }
        if (!prepared.upload_payload.has_value())
        {
            return fail(ErrorKind::Internal, "prepared upload-routine has no payload");
        }
        const Status uploaded = upload(context, outcome, slot->ram_address, *prepared.upload_payload);
        if (!uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
        outcome.note =
            std::format("uploaded {} routine bytes to 0x{:06x}", prepared.upload_payload->size(), slot->ram_address);
        break;
    }
    }

    return {};
}

CommandOutcome run_step(BenchContext& context, const PreparedStep& prepared)
{
    CommandOutcome outcome;
    outcome.step = renderStep(prepared.spec);

    const Status result = executeStep(context, prepared, outcome);
    if (!result.has_value())
    {
        outcome.ok = false;
        outcome.error_kind = result.error().kind;
        outcome.error_detail = result.error().detail;
    }

    const Result<double> battery = context.session.vbatt();
    if (battery.has_value())
    {
        outcome.vbatt = *battery;
    }
    return outcome;
}

CommandOutcome run_step(BenchContext& context, const StepSpec& step)
{
    const Result<PreparedStep> prepared = prepare_step(context.files, step);
    if (prepared.has_value())
    {
        return run_step(context, *prepared);
    }

    CommandOutcome outcome{.step = renderStep(step),
                           .ok = false,
                           .error_kind = prepared.error().kind,
                           .error_detail = prepared.error().detail};
    const Result<double> battery = context.session.vbatt();
    if (battery.has_value())
    {
        outcome.vbatt = *battery;
    }
    return outcome;
}

} // namespace fastecu::bench
