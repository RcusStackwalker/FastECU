#pragma once

#include <format>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// The operation a single-image CAN bootloader family runs once its transport
// is up: connect to the bootloader, then either read the plan's transfer
// region or write the plan's image. The family owns every exchange and the two
// phases `write` starts; this owns the "Connect" and "Read ROM" phases, the
// operator notices and log lines, the TestWrite refusal and the result's
// shape. `unit` names the bootloader in those texts, e.g.
// "Subaru Hitachi M32R".
//
//   connect() -> Status
//   read(PhaseReporter&) -> Result<bytes::Bytes>, the complete read image
//   write(bytes::ByteView image, PhaseSequence&) -> Status
template <typename Connect, typename Read, typename Write>
Result<FlashExecutionResult> RunBootloaderOperation(const FlashPlan& plan, IEventSink& events, std::string_view unit,
                                                    Connect connect, Read read, Write write)
{
    const bool reading = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, reading ? 2 : 3);
    PhaseReporter connecting = phases.Start("Connect", 1);
    events.Log(LogLevel::kInfo, std::format("Connecting to {} CAN bootloader, please wait...", unit));
    if (const Status connected = connect(); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connecting.Complete();

    if (reading)
    {
        events.Notice("Reading ROM, please wait...");
        events.Log(LogLevel::kInfo, std::format("Reading ROM from {} using CAN", unit));
        PhaseReporter read_phase = phases.Start("Read ROM", static_cast<int>(plan.TransferRegion().length));
        Result<bytes::Bytes> rom = read(read_phase);
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        read_phase.Complete();
        return FlashExecutionResult{
            .operation = FlashOperation::kRead,
            .read_bytes = std::move(*rom),
        };
    }

    // Each family's plan builder refuses TestWrite; the guard is repeated
    // here so a plan built another way cannot turn a dry run into a real
    // erase and write. It cannot fire while plan validation rejects TestWrite
    // before any I/O, and is kept as the last thing between a non-Write
    // operation and a real erase-and-write should that validation ever be
    // relaxed or the enum gain a value. Do not delete it as "dead code".
    if (plan.Operation() != FlashOperation::kWrite)
    {
        return Fail(ErrorKind::kUnsupported, std::format("test_write is not supported by the {} CAN family", unit));
    }

    events.Notice("Writing ROM, please wait...");
    events.Log(LogLevel::kInfo, std::format("Writing ROM to {} using CAN", unit));
    if (const Status written = write(plan.ImageOrEmpty(), phases); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{
        .operation = FlashOperation::kWrite,
        .read_bytes = std::nullopt,
    };
}

} // namespace fastecu::flash
