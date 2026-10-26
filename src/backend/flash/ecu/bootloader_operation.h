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
// region or erase the flash and write the plan's image over that region. The
// family owns every exchange; this owns the phases ("Connect", then "Read ROM"
// or "Erase" and "Write ROM"), the operator notices and log lines, the
// TestWrite refusal and the result's shape. `unit` names the bootloader in those texts, e.g.
// "Subaru Hitachi M32R".
//
// Each step takes the family's executor context, whose event sink carries
// the phases, notices and log lines:
//   connect(ctx) -> Status
//   read(ctx, PhaseReporter&) -> Result<bytes::Bytes>, the complete read image
//   erase(ctx) -> Status
//   write(ctx, bytes::ByteView image, PhaseReporter&) -> Status, given the
//   whole plan image and reporting progress over the transfer region
template <typename Ctx, typename Connect, typename Read, typename Erase, typename Write>
Result<FlashExecutionResult> RunBootloaderOperation(const FlashPlan& plan, std::string_view unit, Ctx& ctx,
                                                    Connect connect, Read read, Erase erase, Write write)
{
    IEventSink& events = ctx.events;
    const bool reading = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, reading ? 2 : 3);
    PhaseReporter connecting = phases.Start("Connect", 1);
    events.Log(LogLevel::kInfo, std::format("Connecting to {} CAN bootloader, please wait...", unit));
    if (const Status connected = connect(ctx); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connecting.Complete();

    if (reading)
    {
        events.Notice("Reading ROM, please wait...");
        events.Log(LogLevel::kInfo, std::format("Reading ROM from {} using CAN", unit));
        PhaseReporter read_phase = phases.Start("Read ROM", static_cast<int>(plan.TransferRegion().length));
        Result<bytes::Bytes> rom = read(ctx, read_phase);
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
    PhaseReporter erasing = phases.Start("Erase", 1);
    if (const Status erased = erase(ctx); !erased.has_value())
    {
        return std::unexpected(erased.error());
    }
    erasing.Complete();
    PhaseReporter writing = phases.Start("Write ROM", static_cast<int>(plan.TransferRegion().length));
    if (const Status written = write(ctx, plan.ImageOrEmpty(), writing); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    writing.Complete();
    return FlashExecutionResult{
        .operation = FlashOperation::kWrite,
        .read_bytes = std::nullopt,
    };
}

} // namespace fastecu::flash
