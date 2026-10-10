#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_map.h"

namespace fastecu::config
{

// How a protocol's ROM checksum is handled before a write. protocols.cfg
// spelled these "yes", "n/a" and "no".
enum class ChecksumSupport
{
    kCorrected, // a checksum module corrects the image
    kMissing,   // a module should exist but does not: warn before writing
    kNone,      // the family has no checksum: write without a warning
};

// The legacy flag text ChecksumSelection still takes: "yes", "n/a" or "no".
std::string_view ChecksumFlag(ChecksumSupport support);

// One memory map a protocol's ROM files can have. A ROM file selects it by its
// exact size; see SelectMemoryMap.
struct MemoryMapSpec
{
    memory::ByteCount file_size;
    std::span<const memory::MemoryBlock> blocks;
    // The ECU address that address 0 in these ROMs' definition files stands
    // for (ADR 0020).
    memory::FlashAddress definition_base;
};

// A ROM-file-backed memory block, for constant catalog data. A range that is
// empty or runs past the 32-bit address space does not compile: a
// throw-expression is not a constant expression.
consteval memory::MemoryBlock FileBlock(std::uint32_t start, std::uint32_t size, std::uint32_t file_offset,
                                        memory::Writability writability)
{
    const auto range =
        memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size});
    return range.has_value()
               ? memory::MemoryBlock{.range = *range,
                                     .backing = memory::FileBacking{.offset = memory::FileOffset{file_offset}},
                                     .writability = writability}
               : throw std::invalid_argument("memory block range is empty or runs past the address space");
}

// A read-only block of `fill` bytes with no ROM file bytes, for constant catalog data.
consteval memory::MemoryBlock FillBlock(std::uint32_t start, std::uint32_t size, std::uint8_t fill)
{
    const auto range =
        memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size});
    return range.has_value()
               ? memory::MemoryBlock{.range = *range,
                                     .backing = memory::FillBacking{.value = fill},
                                     .writability = memory::Writability::kReadOnly}
               : throw std::invalid_argument("memory block range is empty or runs past the address space");
}

// One flash and logging protocol, as compile-time data.
struct ProtocolSpec
{
    std::string_view name;  // unique within a catalog
    std::string_view alias; // one token a definition's flash method may use; empty when none
    std::string_view ecu;
    std::string_view mcu; // names the FlashDevice (FindFlashDevice) giving erase geometry
    std::string_view mode;
    ChecksumSupport checksum = ChecksumSupport::kNone;
    bool read = false;
    bool test_write = false;
    bool write = false;
    std::string_view flash_transport; // comma-separated; the UI splits it
    std::string_view log_transport;   // comma-separated; the UI splits it
    std::string_view log_protocol;
    std::string_view kernel; // a file in the kernel directory; empty when none is uploaded
    std::optional<std::uint32_t> kernel_load_address;
    std::string_view description;
    // Empty when ROM files sit at address 0 with every byte writable: those get
    // the identity map at any size. Otherwise one map per ROM file size the
    // protocol accepts.
    std::span<const MemoryMapSpec> memory_maps;
};

// The load address as protocols.cfg spelled it -- "0x" then unpadded
// uppercase hex, e.g. "0xFFFF3000" -- or "" when there is none.
std::string KernelLoadAddressText(const ProtocolSpec& protocol);

// The memory map of a `file_size`-byte ROM file for `protocol`: the declared map
// of exactly that size, or the identity map when the protocol declares none.
// kFileSizeMismatch when it declares maps but none of that size; a ROM file is
// never padded to fit one.
std::expected<memory::MemoryMap, memory::MemoryError> SelectMemoryMap(const ProtocolSpec& protocol,
                                                                      memory::ByteCount file_size);

// One vehicle the operator can select.
struct VehicleSpec
{
    std::string_view id; // saved in fastecu.cfg: never changed, never reused
    std::string_view make;
    std::string_view model;
    std::string_view version;
    std::string_view type;
    std::string_view kw;
    std::string_view hp;
    std::string_view fuel;
    std::string_view year;
    // Into the same catalog's protocols; never null in a consistent catalog.
    const ProtocolSpec *protocol = nullptr;
};

// The protocol named `name`, for wiring VehicleSpec::protocol in constant
// data; nullptr when `protocols` has none, which catalog_references_resolve
// then rejects.
consteval const ProtocolSpec *ProtocolIn(std::span<const ProtocolSpec> protocols, std::string_view name)
{
    const auto found = std::ranges::find(protocols, name, &ProtocolSpec::name);
    return found == protocols.end() ? nullptr : &*found;
}

// The checks cheap enough to static_assert on every supported compiler:
// every vehicle has a protocol, every protocol has a vehicle, and no protocol
// has a kernel load address without a kernel. catalog_problems() checks
// these and the rest.
constexpr bool CatalogReferencesResolve(std::span<const ProtocolSpec> protocols, std::span<const VehicleSpec> vehicles)
{
    const auto has_protocol = [](const VehicleSpec& vehicle) { return vehicle.protocol != nullptr; };
    const auto reachable = [vehicles](const ProtocolSpec& protocol)
    {
        return std::ranges::any_of(vehicles,
                                   [&protocol](const VehicleSpec& vehicle) { return vehicle.protocol == &protocol; });
    };
    const auto kernel_named = [](const ProtocolSpec& protocol)
    { return !protocol.kernel_load_address.has_value() || !protocol.kernel.empty(); };
    return std::ranges::all_of(vehicles, has_protocol) && std::ranges::all_of(protocols, reachable) &&
           std::ranges::all_of(protocols, kernel_named);
}

// A protocol and vehicle table the application runs against. Holds views:
// the arrays it is built from must outlive it.
class Catalog
{
  public:
    constexpr Catalog(std::span<const ProtocolSpec> protocols, std::span<const VehicleSpec> vehicles) noexcept
        : protocols_(protocols), vehicles_(vehicles)
    {
    }

    constexpr std::span<const ProtocolSpec> Protocols() const noexcept
    {
        return protocols_;
    }
    // Presentation order; a vehicle's row is its position.
    constexpr std::span<const VehicleSpec> Vehicles() const noexcept
    {
        return vehicles_;
    }

    // nullptr when no protocol is named `name`.
    const ProtocolSpec *FindProtocol(std::string_view name) const;
    // The row of the vehicle whose id is `id`.
    std::optional<std::size_t> FindVehicle(std::string_view id) const;
    // The LAST row whose protocol is named `protocol_name`: the rule ROM open
    // has always used to select a vehicle from a flash method.
    std::optional<std::size_t> LastVehicleForProtocol(std::string_view protocol_name) const;
    // The FIRST vehicle whose protocol's alias is `flash_method`: the rule a
    // definition's flash method has always been resolved by. An empty
    // `flash_method` matches nothing.
    const VehicleSpec *FirstVehicleForAlias(std::string_view flash_method) const;

  private:
    std::span<const ProtocolSpec> protocols_;
    std::span<const VehicleSpec> vehicles_;
};

// Every way `catalog` is inconsistent, one message each; empty when it is
// consistent. Covers catalog_references_resolve, plus duplicate protocol
// names and vehicle ids, vehicle id spelling, comma-separated aliases,
// vehicles whose protocol belongs to another catalog, and memory maps that are
// invalid or share a ROM file size.
std::vector<std::string> CatalogProblems(const Catalog& catalog);

} // namespace fastecu::config
