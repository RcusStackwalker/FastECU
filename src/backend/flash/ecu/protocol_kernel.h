#pragma once

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/config/catalog.h"
#include "src/backend/config/config_paths.h"
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

} // namespace fastecu::flash
