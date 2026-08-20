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
// MitsuColtCan::kFlashReadBlockSize, filling outcome.tx/rx/note. A reply
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

    std::uint32_t offset = 0;
    int chunk_count = 0;
    while (offset < *len)
    {
        const std::uint32_t remaining = *len - offset;
        const auto chunk_len =
            static_cast<bytes::Byte>(std::min<std::uint32_t>(remaining, MitsuColtCan::kFlashReadBlockSize));
        const bytes::Bytes pdu = MitsuColtCan::buildReadMemoryByAddress(*addr + offset, chunk_len);
        if (chunk_count == 0)
        {
            outcome.tx = pdu;
        }

        const Result<bytes::Bytes> reply = context.session.exchange(pdu, kRoutinePolicy);
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
        outcome.rx.insert(outcome.rx.end(), payload.begin(), payload.begin() + chunk_len);

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
// `addr`. All destructive callers use kSlowPolicy -- flash writes and the
// erase they depend on are not 500ms operations.
Status upload(BenchContext& context, std::uint32_t addr, bytes::ByteView payload)
{
    const bytes::Bytes requestDownload =
        MitsuColtCan::buildRequestDownload(addr, static_cast<std::uint32_t>(payload.size()));
    if (const Result<bytes::Bytes> reply = context.session.exchange(requestDownload, kSlowPolicy); !reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    for (const bytes::Bytes& frame : MitsuColtCan::buildTransferDataFrames(payload))
    {
        if (const Result<bytes::Bytes> reply = context.session.exchange(frame, kSlowPolicy); !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
    }

    const bytes::Bytes crcRequestDownload =
        MitsuColtCan::buildRequestDownload(MitsuColtCan::kCrcTransferAddress, MitsuColtCan::kCrcTransferSize);
    if (const Result<bytes::Bytes> reply = context.session.exchange(crcRequestDownload, kSlowPolicy);
        !reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::Bytes checksumBytes = bytes::composeBe(MitsuColtCan::checksum(payload));
    for (const bytes::Bytes& frame : MitsuColtCan::buildTransferDataFrames(checksumBytes))
    {
        if (const Result<bytes::Bytes> reply = context.session.exchange(frame, kSlowPolicy); !reply.has_value())
        {
            return std::unexpected(reply.error());
        }
    }

    const bytes::Bytes crcCheck = MitsuColtCan::buildRoutineCheckCrc(addr);
    const Result<bytes::Bytes> crcReply = context.session.exchange(crcCheck, kSlowPolicy);
    if (!crcReply.has_value())
    {
        return std::unexpected(crcReply.error());
    }
    const bytes::ByteView crcPayload = uds::payload(*crcReply);
    if (crcPayload.size() < 2 || crcPayload[1] != 0)
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
    return payload[1] == 0 ? "status=0x00 (CRC matched)" : std::format("status=0x{:02x} (CRC mismatch)", payload[1]);
}

Result<CommandOutcome> run_step(BenchContext& context, const StepSpec& step)
{
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

    CommandOutcome outcome;
    outcome.step = renderStep(step);

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
        const Status saved = context.files.save(step.args[2], outcome.rx);
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
        const bytes::Bytes pdu = MitsuColtCan::buildRoutineCheckCrc(*addr);
        outcome.tx = pdu;
        const Result<bytes::Bytes> reply = context.session.exchange(pdu, kSlowPolicy);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        const bytes::ByteView payload = uds::payload(*reply);
        outcome.rx.assign(payload.begin(), payload.end());
        outcome.note = decode_crc_reply(payload);
        if (payload.size() < 2 || payload[1] != 0)
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
        outcome.tx = *pdu;
        const Result<bytes::Bytes> reply = context.session.exchange(*pdu, kRoutinePolicy);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        outcome.rx = *reply;
        break;
    }
    case CommandId::SendRaw:
    {
        const Result<bytes::Bytes> pdu = parse_hex_bytes(step.args);
        if (!pdu.has_value())
        {
            return std::unexpected(pdu.error());
        }
        outcome.tx = *pdu;
        // exchange_raw bypasses SID/NRC validation entirely -- whatever comes
        // back is the observation the operator asked for, not something to
        // classify as success or failure by content. A genuine transport
        // error (nothing arrived at all) still propagates like every other
        // command's Result does.
        const Result<bytes::Bytes> reply = context.session.exchange_raw(*pdu, context.options.timeout_ms);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        outcome.rx = *reply;
        break;
    }
    case CommandId::Connect:
    {
        const Status connected = context.session.connect();
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
        outcome.tx = pdu;
        const Result<bytes::Bytes> reply = context.session.exchange(pdu, kSlowPolicy);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        outcome.rx = *reply;
        break;
    }
    case CommandId::Erase:
    {
        const bytes::Bytes pdu = MitsuColtCan::buildRoutineErase();
        outcome.tx = pdu;
        const Result<bytes::Bytes> reply = context.session.exchange(pdu, kSlowPolicy);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        const bytes::ByteView payload = uds::payload(*reply);
        outcome.rx.assign(payload.begin(), payload.end());
        outcome.note = decode_erase_reply(payload);
        if (payload.size() < 2 || payload[1] != 0)
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
        const Result<bytes::Bytes> data = context.files.load(step.args[1]);
        if (!data.has_value())
        {
            return std::unexpected(data.error());
        }
        const Status uploaded = upload(context, *addr, *data);
        if (!uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
        outcome.note = std::format("uploaded {} bytes to 0x{:06x}", data->size(), *addr);
        break;
    }
    case CommandId::UploadRoutine:
    {
        const Result<RoutineSlot> slot = routine_slot(step.args[0]);
        if (!slot.has_value())
        {
            return std::unexpected(slot.error());
        }
        // Default to the baked array; --from <file> substitutes the file's
        // bytes for provenance-testing a rebuilt routine, while the RAM slot
        // (address) always follows the routine name, not the source of bytes.
        // A malformed shape (flag typo, or the path given without the flag)
        // is rejected rather than silently falling back to the baked array --
        // this path exists specifically to prove which bytes went out.
        bytes::ByteView payload = slot->bytes;
        bytes::Bytes fileBytes;
        if (step.args.size() > 1)
        {
            if (step.args.size() != 3 || step.args[1] != "--from")
            {
                return fail(ErrorKind::InvalidConfig,
                            std::format("{}'s extra arguments must be --from <path>", spec->name));
            }
            const Result<bytes::Bytes> loaded = context.files.load(step.args[2]);
            if (!loaded.has_value())
            {
                return std::unexpected(loaded.error());
            }
            fileBytes = *loaded;
            payload = fileBytes;
        }
        const Status uploaded = upload(context, slot->ram_address, payload);
        if (!uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
        outcome.note = std::format("uploaded {} routine bytes to 0x{:06x}", payload.size(), slot->ram_address);
        break;
    }
    }

    const Result<double> battery = context.session.vbatt();
    if (battery.has_value())
    {
        outcome.vbatt = *battery;
    }
    return outcome;
}

} // namespace fastecu::bench
