#pragma once

#include <optional>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

// The protocol's kernel file, as the kernel image a plan carries: id
// "<protocol>-kernel", loaded at the protocol's kernel load address. Fails
// when the protocol declares no load address; a failed file read passes the
// repository's error through unchanged.
Result<KernelImage> ReadProtocolKernel(const config::ProtocolSpec& protocol, const config::ConfigPaths& paths,
                                       IFileRepository& files);

// The protocol's kernel file alone, for a family whose boot ROM places the
// kernel itself (the Unisia Jecs M32R _bootmode entries declare no load
// address). A failed read names the kernel file.
Result<bytes::Bytes> ReadProtocolKernelBytes(const config::ProtocolSpec& protocol, const config::ConfigPaths& paths,
                                             IFileRepository& files);

// A family builder that is handed the protocol's kernel image.
using KernelBackedPlanBuilder = Result<FlashPlan> (*)(FlashOperation, std::string_view, std::string_view,
                                                      std::optional<bytes::Bytes>, KernelImage);

// Plan preparation for a kernel-backed family: the protocol's kernel file is
// read, then the family builder runs with it and the ROM file.
template <KernelBackedPlanBuilder Build>
Result<FlashPlan> PrepareWithProtocolKernel(FlashOperation operation, const config::ProtocolSpec& protocol,
                                            const config::ConfigPaths& paths, std::optional<bytes::Bytes> rom_file,
                                            IFileRepository& files)
{
    return ReadProtocolKernel(protocol, paths, files)
        .and_then([&](KernelImage kernel)
                  { return Build(operation, protocol.name, protocol.mcu, std::move(rom_file), std::move(kernel)); });
}

} // namespace fastecu::flash
