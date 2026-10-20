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
    CommandSpec{CommandId::kPorts, "ports", false, 0, 0},
    CommandSpec{CommandId::kConnect, "connect", false, 0, 0},
    CommandSpec{CommandId::kRead, "read", false, 2, 2},
    CommandSpec{CommandId::kDump, "dump", false, 3, 3},
    CommandSpec{CommandId::kCrcCheck, "crc-check", false, 1, 1},
    CommandSpec{CommandId::kSend, "send", true, 1, kUnbounded},
    CommandSpec{CommandId::kSendRaw, "send-raw", true, 1, kUnbounded},
    CommandSpec{CommandId::kUnlock, "unlock", true, 0, 0},
    CommandSpec{CommandId::kErase, "erase", true, 0, 0},
    CommandSpec{CommandId::kDownload, "download", true, 2, 2},
    CommandSpec{CommandId::kUploadRoutine, "upload-routine", true, 1, 3}, // routine name, plus optional --from <path>
};

} // namespace

std::span<const CommandSpec> CommandTable()
{
    return kCommands;
}

const CommandSpec *FindCommand(std::string_view name)
{
    const auto found = std::ranges::find(kCommands, name, &CommandSpec::name);
    return found == kCommands.end() ? nullptr : std::to_address(found);
}

const CommandSpec *FindCommand(CommandId id)
{
    const auto found = std::ranges::find(kCommands, id, &CommandSpec::id);
    return found == kCommands.end() ? nullptr : std::to_address(found);
}

Status ValidateAgainstTable(const CommandSpec& spec, const StepSpec& step)
{
    if (step.args.size() < spec.min_args || (spec.max_args != kUnbounded && step.args.size() > spec.max_args))
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} takes {}..{} arguments, got {}", spec.name, spec.min_args,
                                spec.max_args == kUnbounded ? std::string("*") : std::to_string(spec.max_args),
                                step.args.size()));
    }
    if (spec.destructive && !step.destructive_ack)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} needs --destructive", spec.name));
    }
    return {};
}

} // namespace fastecu::bench
