#include "src/backend/flash/ecu/protocol_kernel.h"

#include <cstdint>
#include <format>
#include <utility>
#include <vector>

namespace fastecu::flash
{

Result<KernelImage> ReadProtocolKernel(const config::ProtocolSpec& protocol, const config::ConfigPaths& paths,
                                       IFileRepository& files)
{
    if (!protocol.kernel_load_address.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("protocol '{}' declares no kernel load address", protocol.name));
    }
    Result<std::vector<std::uint8_t>> kernel_bytes = files.Read(config::KernelFileHandle(paths, protocol.kernel));
    if (!kernel_bytes.has_value())
    {
        return std::unexpected(kernel_bytes.error());
    }
    return KernelImage{.id = std::format("{}-kernel", protocol.name),
                       .load_address = *protocol.kernel_load_address,
                       .bytes = std::move(*kernel_bytes)};
}

Result<bytes::Bytes> ReadProtocolKernelBytes(const config::ProtocolSpec& protocol, const config::ConfigPaths& paths,
                                             IFileRepository& files)
{
    Result<std::vector<std::uint8_t>> kernel_bytes = files.Read(config::KernelFileHandle(paths, protocol.kernel));
    if (!kernel_bytes.has_value())
    {
        const Error& error = kernel_bytes.error();
        return Fail(error.kind, std::format("kernel file '{}': {}", protocol.kernel, error.detail));
    }
    return bytes::Bytes(kernel_bytes->begin(), kernel_bytes->end());
}

} // namespace fastecu::flash
