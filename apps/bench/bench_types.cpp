#include "apps/bench/bench_types.h"

#include <algorithm>
#include <array>

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
    CommandSpec{CommandId::Send, "send", false, 1, kUnbounded},
    CommandSpec{CommandId::SendRaw, "send-raw", false, 1, kUnbounded},
    CommandSpec{CommandId::Unlock, "unlock", true, 0, 0},
    CommandSpec{CommandId::Erase, "erase", true, 0, 0},
    CommandSpec{CommandId::Download, "download", true, 2, 2},
    CommandSpec{CommandId::UploadRoutine, "upload-routine", true, 1, 1},
};

} // namespace

std::span<const CommandSpec> command_table()
{
    return kCommands;
}

const CommandSpec *find_command(std::string_view name)
{
    const auto found = std::ranges::find(kCommands, name, &CommandSpec::name);
    return found == kCommands.end() ? nullptr : &*found;
}

} // namespace fastecu::bench
