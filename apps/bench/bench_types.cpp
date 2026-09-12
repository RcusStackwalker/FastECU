#include "apps/bench/bench_types.h"

#include <algorithm>
#include <format>
#include <array>
#include <memory>

namespace fastecu::bench
{
namespace
{

constexpr std::array kCommands = {
    CommandSpec{CommandId::Ports, "ports", false, 0, 0},
    CommandSpec{CommandId::Connect, "connect", false, 0, 0},
    CommandSpec{CommandId::Read, "read", false, 2, 2},
    CommandSpec{CommandId::Dump, "dump", false, 3, 3},
    CommandSpec{CommandId::CrcCheck, "crc-check", false, 1, 1},
    CommandSpec{CommandId::Send, "send", true, 1, kUnbounded},
    CommandSpec{CommandId::SendRaw, "send-raw", true, 1, kUnbounded},
    CommandSpec{CommandId::Unlock, "unlock", true, 0, 0},
    CommandSpec{CommandId::Erase, "erase", true, 0, 0},
    CommandSpec{CommandId::Download, "download", true, 2, 2},
    CommandSpec{CommandId::UploadRoutine, "upload-routine", true, 1, 3}, // routine name, plus optional --from <path>
};

} // namespace

std::span<const CommandSpec> command_table()
{
    return kCommands;
}

const CommandSpec *find_command(std::string_view name)
{
    const auto found = std::ranges::find(kCommands, name, &CommandSpec::name);
    return found == kCommands.end() ? nullptr : std::to_address(found);
}

const CommandSpec *find_command(CommandId id)
{
    const auto found = std::ranges::find(kCommands, id, &CommandSpec::id);
    return found == kCommands.end() ? nullptr : std::to_address(found);
}

Status validate_against_table(const CommandSpec& spec, const StepSpec& step)
{
    if (step.args.size() < spec.min_args || (spec.max_args != kUnbounded && step.args.size() > spec.max_args))
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("{} takes {}..{} arguments, got {}", spec.name, spec.min_args,
                                spec.max_args == kUnbounded ? std::string("*") : std::to_string(spec.max_args),
                                step.args.size()));
    }
    if (spec.destructive && !step.destructive_ack)
    {
        return fail(ErrorKind::InvalidConfig, std::format("{} needs --destructive", spec.name));
    }
    return {};
}

} // namespace fastecu::bench
