#include "src/backend/config/catalog.h"

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fastecu::config
{
namespace
{

bool IsVehicleId(std::string_view id)
{
    return !id.empty() &&
           std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

void AddMemoryMapProblems(const ProtocolSpec& protocol, std::vector<std::string>& problems)
{
    const std::span<const MemoryMapSpec> maps = protocol.memory_maps;
    for (std::size_t index = 0; index < maps.size(); ++index)
    {
        const MemoryMapSpec& spec = maps[index];
        if (const auto map = memory::MemoryMap::Create(spec.blocks, spec.file_size, spec.definition_base);
            !map.has_value())
        {
            problems.push_back(std::format("protocol '{}' memory map for 0x{:x}-byte files is invalid: {}",
                                           protocol.name, spec.file_size.Value(), map.error().detail));
        }
        if (std::ranges::any_of(maps.first(index),
                                [&spec](const MemoryMapSpec& earlier) { return earlier.file_size == spec.file_size; }))
        {
            problems.push_back(std::format("protocol '{}' declares two memory maps for 0x{:x}-byte files",
                                           protocol.name, spec.file_size.Value()));
        }
    }
}

void AddProtocolProblems(const Catalog& catalog, std::vector<std::string>& problems)
{
    const std::span<const ProtocolSpec> protocols = catalog.Protocols();
    for (std::size_t index = 0; index < protocols.size(); ++index)
    {
        const ProtocolSpec& protocol = protocols[index];
        const std::span<const ProtocolSpec> earlier = protocols.first(index);
        if (protocol.name.empty())
        {
            problems.emplace_back("protocol name is empty");
        }
        else if (std::ranges::find(earlier, protocol.name, &ProtocolSpec::name) != earlier.end())
        {
            problems.push_back(std::format("duplicate protocol name '{}'", protocol.name));
        }
        if (protocol.alias.contains(','))
        {
            problems.push_back(std::format("protocol '{}' alias '{}' contains ','", protocol.name, protocol.alias));
        }
        if (protocol.kernel_load_address.has_value() && protocol.kernel.empty())
        {
            problems.push_back(std::format("protocol '{}' has a kernel load address but no kernel", protocol.name));
        }
        if (std::ranges::none_of(catalog.Vehicles(),
                                 [&protocol](const VehicleSpec& vehicle) { return vehicle.protocol == &protocol; }))
        {
            problems.push_back(std::format("protocol '{}' has no vehicle", protocol.name));
        }
        AddMemoryMapProblems(protocol, problems);
    }
}

void AddVehicleProblems(const Catalog& catalog, std::vector<std::string>& problems)
{
    const std::span<const VehicleSpec> vehicles = catalog.Vehicles();
    for (std::size_t index = 0; index < vehicles.size(); ++index)
    {
        const VehicleSpec& vehicle = vehicles[index];
        const std::span<const VehicleSpec> earlier = vehicles.first(index);
        if (!IsVehicleId(vehicle.id))
        {
            problems.push_back(std::format("vehicle id '{}' is not lowercase [a-z0-9-]", vehicle.id));
        }
        else if (std::ranges::find(earlier, vehicle.id, &VehicleSpec::id) != earlier.end())
        {
            problems.push_back(std::format("duplicate vehicle id '{}'", vehicle.id));
        }
        if (vehicle.protocol == nullptr)
        {
            problems.push_back(std::format("vehicle '{}' has no protocol", vehicle.id));
        }
        else if (std::ranges::none_of(catalog.Protocols(), [&vehicle](const ProtocolSpec& protocol)
                                      { return &protocol == vehicle.protocol; }))
        {
            problems.push_back(std::format("vehicle '{}' protocol is not in this catalog", vehicle.id));
        }
    }
}

} // namespace

std::string_view ChecksumFlag(ChecksumSupport support)
{
    switch (support)
    {
    case ChecksumSupport::kCorrected:
        return "yes";
    case ChecksumSupport::kMissing:
        return "n/a";
    case ChecksumSupport::kNone:
        return "no";
    }
    std::unreachable();
}

std::string KernelLoadAddressText(const ProtocolSpec& protocol)
{
    return protocol.kernel_load_address.has_value() ? std::format("0x{:X}", *protocol.kernel_load_address)
                                                    : std::string{};
}

std::expected<memory::MemoryMap, memory::MemoryError> SelectMemoryMap(const ProtocolSpec& protocol,
                                                                      memory::ByteCount file_size)
{
    if (protocol.memory_maps.empty())
    {
        return memory::MemoryMap::Identity(file_size);
    }
    const auto found = std::ranges::find(protocol.memory_maps, file_size, &MemoryMapSpec::file_size);
    if (found == protocol.memory_maps.end())
    {
        return std::unexpected(
            memory::MemoryError{.kind = memory::MemoryErrorKind::kFileSizeMismatch,
                                .detail = std::format("protocol '{}' has no memory map for 0x{:x}-byte ROM files",
                                                      protocol.name, file_size.Value())});
    }
    return memory::MemoryMap::Create(found->blocks, found->file_size, found->definition_base);
}

const ProtocolSpec *Catalog::FindProtocol(std::string_view name) const
{
    const auto found = std::ranges::find(protocols_, name, &ProtocolSpec::name);
    return found == protocols_.end() ? nullptr : &*found;
}

std::optional<std::size_t> Catalog::FindVehicle(std::string_view id) const
{
    const auto found = std::ranges::find(vehicles_, id, &VehicleSpec::id);
    if (id.empty() || found == vehicles_.end())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::ranges::distance(vehicles_.begin(), found));
}

std::optional<std::size_t> Catalog::LastVehicleForProtocol(std::string_view protocol_name) const
{
    std::optional<std::size_t> last;
    for (std::size_t row = 0; row < vehicles_.size(); ++row)
    {
        if (vehicles_[row].protocol != nullptr && vehicles_[row].protocol->name == protocol_name)
        {
            last = row;
        }
    }
    return last;
}

const VehicleSpec *Catalog::FirstVehicleForAlias(std::string_view flash_method) const
{
    if (flash_method.empty())
    {
        return nullptr;
    }
    const auto found =
        std::ranges::find_if(vehicles_, [flash_method](const VehicleSpec& vehicle)
                             { return vehicle.protocol != nullptr && vehicle.protocol->alias == flash_method; });
    return found == vehicles_.end() ? nullptr : &*found;
}

std::vector<std::string> CatalogProblems(const Catalog& catalog)
{
    std::vector<std::string> problems;
    AddProtocolProblems(catalog, problems);
    AddVehicleProblems(catalog, problems);
    return problems;
}

} // namespace fastecu::config
