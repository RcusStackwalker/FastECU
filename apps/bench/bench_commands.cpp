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
using namespace std::chrono_literals;

constexpr uds::ExchangePolicy kRoutinePolicy{.read_timeout = 500ms};
constexpr uds::ExchangePolicy kSlowPolicy{.read_timeout = 3000ms};
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
    if (const std::uint64_t last = static_cast<std::uint64_t>(address) + length - 1; last > kMaxWireU24)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} range 0x{:x}..0x{:x} exceeds the 24-bit address space", subject, address, last));
    }
    return {};
}

Result<bytes::Bytes> exchange(BenchContext& context, CommandOutcome& outcome, bytes::ByteView pdu,
                              const uds::ExchangePolicy& policy)
{
    Result<bytes::Bytes> result = context.session.exchange(pdu, policy);
    append_traffic(outcome, context.session.last_traffic());
    return result;
}

Result<bytes::Bytes> exchangeRaw(BenchContext& context, CommandOutcome& outcome, bytes::ByteView pdu, int timeout_ms)
{
    Result<bytes::Bytes> result = context.session.exchange_raw(pdu, timeout_ms);
    append_traffic(outcome, context.session.last_traffic());
    return result;
}

Status connect(BenchContext& context, CommandOutcome& outcome)
{
    Status result = context.session.connect();
    append_traffic(outcome, context.session.last_traffic());
    return result;
}

// Shared by Read and Dump: chunks [addr, addr+len) at
// MitsuColtCan::kFlashReadBlockSize, filling outcome.data/note and traffic. A reply
// shorter than the requested chunk is rejected rather than padded, since a
// silently truncated read would look like a shorter-than-requested memory
// region instead of the protocol error it is.
Status readIntoOutcome(BenchContext& context, const PreparedStep& prepared, CommandOutcome& outcome)
{
    const std::uint32_t addr = prepared.address;
    const std::uint32_t len = prepared.length;
    // Re-checked at the wire, not re-parsed: prepare_step decoded these, but an
    // address-window guard is worth keeping at the last gate before the ECU.
    if (const Status valid = validateWireRange(addr, len, "read"); !valid.has_value())
    {
        return valid;
    }

    outcome.data.reserve(len);
    std::uint32_t offset = 0;
    int chunk_count = 0;
    while (offset < len)
    {
        const std::uint32_t remaining = len - offset;
        const auto chunk_len =
            static_cast<bytes::Byte>(std::min<std::uint32_t>(remaining, MitsuColtCan::kFlashReadBlockSize));
        const bytes::Bytes pdu = MitsuColtCan::buildReadMemoryByAddress(addr + offset, chunk_len);
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
    // Uploading this routine is what arms `erase`; see the erase prerequisite
    // chain in bench_driver.
    bool erase_helper = false;
};

Result<RoutineSlot> routine_slot(std::string_view name)
{
    using namespace MitsuColtCan;
    if (name == "erase-page")
    {
        return RoutineSlot{name, kErasePageRoutine, kEraseRoutineRamAddr, true};
    }
    if (name == "erase-redirect")
    {
        return RoutineSlot{name, kEraseRedirectRoutine, kEraseRoutineRamAddr, true};
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
    // RequestDownload for the block, then its TransferData frames. The payload
    // and the checksum go the same way, so they go through the same sequence.
    const auto transfer = [&](std::uint32_t at, bytes::ByteView block) -> Status
    {
        const bytes::Bytes request = MitsuColtCan::buildRequestDownload(at, static_cast<std::uint32_t>(block.size()));
        if (const Result<bytes::Bytes> reply = exchange(context, outcome, request, kRoutinePolicy); !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        for (const bytes::Bytes& frame : MitsuColtCan::buildTransferDataFrames(block))
        {
            if (const Result<bytes::Bytes> reply = exchange(context, outcome, frame, kRoutinePolicy);
                !reply.has_value())
            {
                return std::unexpected(reply.error());
            }
        }
        return {};
    };

    if (const Status sent = transfer(addr, payload); !sent.has_value())
    {
        return sent;
    }
    // kCrcTransferSize bytes by construction: checksum() is a uint16_t.
    const bytes::Bytes checksumBytes = bytes::composeBe(MitsuColtCan::checksum(payload));
    if (const Status sent = transfer(MitsuColtCan::kCrcTransferAddress, checksumBytes); !sent.has_value())
    {
        return sent;
    }

    const bytes::Bytes crcCheck = MitsuColtCan::buildRoutineCheckCrc(addr);
    const Result<bytes::Bytes> crcReply = exchange(context, outcome, crcCheck, kSlowPolicy);
    if (!crcReply.has_value())
    {
        return std::unexpected(crcReply.error());
    }
    if (const bytes::ByteView crcPayload = uds::payload(*crcReply);
        crcPayload.size() < 2 || crcPayload[0] != MitsuColtCan::kRoutineCheckCrc || crcPayload[1] != 0)
    {
        return fail(ErrorKind::BadResponse, decode_crc_reply(crcPayload));
    }
    return {};
}

// RoutineControl whose reply carries [routine-id][status]: exchange on the slow
// policy, record the decoded reply as the note, and fail on any status but 0.
// Shared by crc-check and erase, whose replies differ only in which routine id
// they echo and how the status decodes.
template <class Decode>
Status routineWithStatus(BenchContext& context, CommandOutcome& outcome, bytes::ByteView pdu, bytes::Byte routine,
                         Decode decode)
{
    const Result<bytes::Bytes> reply = exchange(context, outcome, pdu, kSlowPolicy);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::payload(*reply);
    outcome.note = decode(payload);
    if (payload.size() < 2 || payload[0] != routine || payload[1] != 0)
    {
        return fail(ErrorKind::BadResponse, outcome.note);
    }
    return {};
}

} // namespace

std::string render_step(const StepSpec& step)
{
    const CommandSpec *const spec = find_command(step.id);
    std::string text{spec == nullptr ? std::string_view{} : spec->name};
    for (const std::string& arg : step.args)
    {
        text += ' ';
        text += arg;
    }
    return text;
}

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
    const CommandSpec *const spec = find_command(step.id);
    if (spec == nullptr)
    {
        return fail(ErrorKind::Internal, "step has no command spec");
    }
    if (const Status valid = validate_against_table(*spec, step); !valid.has_value())
    {
        return std::unexpected(valid.error());
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
        if (const Status valid = validateWireRange(*address, *length, "read"); !valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        prepared.address = *address;
        prepared.length = *length;
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
        prepared.address = *address;
        return prepared;
    }
    case CommandId::Send:
    case CommandId::SendRaw:
    {
        Result<bytes::Bytes> pdu = parse_hex_bytes(step.args);
        if (!pdu.has_value())
        {
            return std::unexpected(pdu.error());
        }
        if (MitsuColtCan::isDestructiveRequest(*pdu))
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("{} cannot bypass a named destructive command", spec->name));
        }
        prepared.pdu = std::move(*pdu);
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
        if (const Status valid = validateWireRange(*address, data->size(), "download"); !valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        prepared.address = *address;
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
        bool from_file = false;
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
            // Only the built-in bytes are a known erase helper; a file could
            // hold anything, so it never arms erase.
            from_file = true;
        }
        if (const Status valid = validateWireRange(slot->ram_address, payload.size(), "download"); !valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        prepared.address = slot->ram_address;
        prepared.upload_payload = std::move(payload);
        prepared.provides_erase_helper = slot->erase_helper && !from_file;
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
    const CommandSpec *const spec = find_command(step.id);
    if (spec == nullptr)
    {
        return fail(ErrorKind::Internal, "step has no command spec");
    }
    // bench_args gates this at parse time; repeated here so a StepSpec built
    // another way cannot reach the wire ungated.
    if (const Status valid = validate_against_table(*spec, step); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }

    switch (step.id)
    {
    case CommandId::Read:
    case CommandId::Dump:
    {
        if (const Status result = readIntoOutcome(context, prepared, outcome); !result.has_value())
        {
            return std::unexpected(result.error());
        }
        if (step.id == CommandId::Dump)
        {
            if (const Status saved = context.files.save(step.args[2], outcome.data); !saved.has_value())
            {
                return std::unexpected(saved.error());
            }
        }
        break;
    }
    case CommandId::CrcCheck:
    {
        // Re-checked at the wire, not re-parsed: prepare_step decoded the
        // address, but an address-window guard belongs at the last gate too.
        if (prepared.address > kMaxWireU24)
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("CRC address 0x{:x} does not fit the 24-bit address space", prepared.address));
        }
        return routineWithStatus(context, outcome, MitsuColtCan::buildRoutineCheckCrc(prepared.address),
                                 MitsuColtCan::kRoutineCheckCrc, decode_crc_reply);
    }
    case CommandId::Send:
    case CommandId::SendRaw:
    {
        // Gated at parse and prepare time as well; repeated here so no PDU
        // reaches the wire without passing the one destructive predicate.
        if (MitsuColtCan::isDestructiveRequest(prepared.pdu))
        {
            return fail(ErrorKind::InvalidConfig,
                        std::format("{} cannot bypass a named destructive command", spec->name));
        }
        // exchange_raw bypasses SID/NRC validation entirely -- whatever comes
        // back is the observation the operator asked for, not something to
        // classify as success or failure by content. A genuine transport
        // error (nothing arrived at all) still propagates like every other
        // command's Result does.
        if (const Result<bytes::Bytes> reply =
                step.id == CommandId::Send ? exchange(context, outcome, prepared.pdu, kRoutinePolicy)
                                           : exchangeRaw(context, outcome, prepared.pdu, context.options.timeout_ms);
            !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        break;
    }
    case CommandId::Connect:
    {
        if (const Status connected = connect(context, outcome); !connected.has_value())
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
        if (const Result<bytes::Bytes> reply = exchange(context, outcome, pdu, kSlowPolicy); !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        break;
    }
    case CommandId::Erase:
        return routineWithStatus(context, outcome, MitsuColtCan::buildRoutineErase(), MitsuColtCan::kRoutineErase,
                                 decode_erase_reply);
    case CommandId::Download:
    case CommandId::UploadRoutine:
    {
        if (!prepared.upload_payload.has_value())
        {
            return fail(ErrorKind::Internal, "prepared upload has no payload");
        }
        if (const Status uploaded = upload(context, outcome, prepared.address, *prepared.upload_payload);
            !uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
        outcome.note = std::format("uploaded {} {}bytes to 0x{:06x}", prepared.upload_payload->size(),
                                   step.id == CommandId::UploadRoutine ? "routine " : "", prepared.address);
        break;
    }
    }

    return {};
}

CommandOutcome run_step(BenchContext& context, const PreparedStep& prepared)
{
    CommandOutcome outcome;
    outcome.step = render_step(prepared.spec);

    if (const Status result = executeStep(context, prepared, outcome); !result.has_value())
    {
        outcome.ok = false;
        outcome.error_kind = result.error().kind;
        outcome.error_detail = result.error().detail;
    }

    if (const Result<double> battery = context.session.vbatt(); battery.has_value())
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

    CommandOutcome outcome{.step = render_step(step),
                           .ok = false,
                           .error_kind = prepared.error().kind,
                           .error_detail = prepared.error().detail};
    if (const Result<double> battery = context.session.vbatt(); battery.has_value())
    {
        outcome.vbatt = *battery;
    }
    return outcome;
}

} // namespace fastecu::bench
