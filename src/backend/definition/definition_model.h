#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/ports/result.h"

namespace fastecu::definition
{

enum class DefinitionFormat
{
    RomRaider,
    EcuFlash,
};

enum class IdEncoding
{
    Ascii,
    Hex,
    AsciiOrHex,
};

enum class StorageType
{
    Uint8,
    Int8,
    Uint16,
    Int16,
    Uint24,
    Int24,
    Uint32,
    Int32,
    Float,
    Bloblist,
};

std::optional<StorageType> storage_type_from_text(std::string_view text);
std::string storage_type_text(std::optional<StorageType> value);
std::uint32_t storage_byte_size(std::optional<StorageType> storage_type);

struct RomDefinition;
struct Scaling;

// The scaling in `definition` named `name`, or nullptr when none matches.
// Hoisted here because the legacy definition adapter and the calibration
// decode both need it; it was privately duplicated in each before.
const Scaling *find_scaling(const RomDefinition& definition, std::string_view name);

// True only for the Uint8/Uint16/Uint24/Uint32 storage types. Reproduces
// legacy's `storagetype.startsWith("uint")` test, including its result for an
// absent storage type (false, since an empty string does not start with
// "uint").
bool is_unsigned_storage(std::optional<StorageType> storage_type);

struct DefinitionIndexEntry
{
    DefinitionFormat format;
    std::string definition_id;
    std::string internal_id;
    std::optional<std::uint64_t> internal_id_address;
    IdEncoding internal_id_encoding;
    std::string ecu_id;
    std::string source;
    std::vector<std::string> parents;

    bool operator==(const DefinitionIndexEntry&) const = default;
};

struct UnresolvedScaling
{
    std::string name;
    std::string units;
    std::optional<std::string> from_byte;
    std::optional<std::string> to_byte;
    std::optional<std::string> format;
    std::string minimum;
    std::string maximum;
    std::string coarse_increment;
    std::string fine_increment;
    std::optional<StorageType> storage_type;
    std::string endian;
    std::vector<std::pair<std::string, std::string>> selections;

    bool operator==(const UnresolvedScaling&) const = default;
};

struct UnresolvedAxisDefinition
{
    std::string type;
    std::string name;
    std::string units;
    std::string format;
    std::optional<StorageType> storage_type;
    std::string endian;
    std::optional<std::uint64_t> address;
    std::optional<std::uint32_t> size;
    std::optional<std::string> from_byte;
    std::optional<std::string> to_byte;
    std::string scaling_name;
    std::optional<std::uint32_t> start_position;
    std::optional<std::uint32_t> interval;
    std::optional<std::string> log_parameter;
    std::optional<std::vector<std::string>> static_data;

    bool operator==(const UnresolvedAxisDefinition&) const = default;
};

struct UnresolvedCalibrationMap
{
    std::optional<std::string> id;
    std::string name;
    std::string type;
    std::string category;
    std::string subcategory;
    std::string description;
    std::optional<std::uint64_t> address;
    std::optional<std::uint32_t> x_size;
    std::optional<std::uint32_t> y_size;
    std::optional<bool> swap_xy;
    std::optional<bool> flip_x;
    std::optional<bool> flip_y;
    std::string level;
    std::string user_level;
    std::string scaling_name;
    std::optional<StorageType> storage_type;
    std::string endian;
    std::optional<std::uint32_t> start_position;
    std::optional<std::uint32_t> interval;
    std::optional<std::string> log_parameter;
    UnresolvedAxisDefinition x_axis;
    UnresolvedAxisDefinition y_axis;

    bool operator==(const UnresolvedCalibrationMap&) const = default;
};

struct Scaling
{
    std::string name;
    std::string units;
    std::string from_byte{"x"};
    std::string to_byte{"x"};
    std::string format;
    std::string minimum;
    std::string maximum;
    std::string coarse_increment;
    std::string fine_increment;
    // nullopt: this scaling takes no position on storage width -- it's purely
    // a display/unit transform (from_byte/to_byte/format/units); the map or
    // axis it's attached to may still have its own storage_type.
    std::optional<StorageType> storage_type;
    std::string endian;
    std::vector<std::pair<std::string, std::string>> selections;

    bool operator==(const Scaling&) const = default;
};

struct AxisDefinition
{
    std::string type;
    std::string name;
    std::string units;
    std::string format;
    // nullopt: this axis doesn't decode bytes from the ROM at all, so no
    // storage width applies -- true for static/literal axes (static_data
    // supplied inline via <data> elements), which likewise have no address.
    std::optional<StorageType> storage_type;
    std::string endian;
    std::optional<std::uint64_t> address;
    std::uint32_t size{1};
    std::string from_byte{"x"};
    std::string to_byte{"x"};
    std::string scaling_name;
    std::uint32_t start_position{1};
    std::uint32_t interval{1};
    std::string log_parameter;
    std::vector<std::string> static_data;

    bool operator==(const AxisDefinition&) const = default;
};

struct CalibrationMap
{
    std::string id;
    std::string name;
    std::string type;
    std::string category;
    std::string subcategory;
    std::string description;
    std::optional<std::uint64_t> address;
    std::uint32_t x_size{1};
    std::uint32_t y_size{1};
    bool swap_xy{false};
    bool flip_x{false};
    bool flip_y{false};
    std::string level;
    std::string user_level;
    std::string scaling_name;
    // nullopt: unlike AxisDefinition, CalibrationMap has no static/literal-data
    // case -- a map always reads real calibration bytes from `address` once
    // resolved. Resolution only checks storage_type for *contradiction*
    // between map and scaling; it never requires one to end up set. So an
    // address-bearing map can resolve with storage_type still nullopt. Not a
    // deliberately valid state, just an unenforced gap -- flagged here rather
    // than fixed.
    std::optional<StorageType> storage_type;
    std::string endian;
    std::uint32_t start_position{1};
    std::uint32_t interval{1};
    std::string log_parameter;
    AxisDefinition x_axis;
    AxisDefinition y_axis;

    bool operator==(const CalibrationMap&) const = default;
};

struct RomIdentity
{
    std::string xml_id;
    std::string internal_id;
    std::string ecu_id;
    std::optional<std::uint64_t> internal_id_address;

    bool operator==(const RomIdentity&) const = default;
};

struct RomMetadata
{
    std::string make;
    std::string market;
    std::string model;
    std::string submodel;
    std::string transmission;
    std::string year;
    std::string flash_method;
    std::string memory_model;
    std::string checksum_module;
    std::string file_size;
    std::string notes;

    bool operator==(const RomMetadata&) const = default;
};

struct UnresolvedDefinition
{
    DefinitionFormat format;
    std::string source;
    RomIdentity identity;
    RomMetadata metadata;
    std::vector<std::string> parents;
    std::vector<UnresolvedCalibrationMap> maps;
    std::vector<UnresolvedScaling> scalings;

    bool operator==(const UnresolvedDefinition&) const = default;
};

struct RomDefinition
{
    DefinitionFormat format;
    std::string source;
    RomIdentity identity;
    RomMetadata metadata;
    std::vector<std::string> parents;
    std::vector<CalibrationMap> maps;
    std::vector<Scaling> scalings;
    std::vector<std::string> resolved_sources;
    std::vector<std::string> resolved_definition_ids;

    bool operator==(const RomDefinition&) const = default;
};

class DefinitionCatalog
{
  public:
    static Result<DefinitionCatalog> create(std::vector<DefinitionIndexEntry> entries);

    Result<std::reference_wrapper<const DefinitionIndexEntry>> find(DefinitionFormat format,
                                                                    std::string_view id) const;

    std::span<const DefinitionIndexEntry> entries() const;

  private:
    explicit DefinitionCatalog(std::vector<DefinitionIndexEntry> entries);

    std::vector<DefinitionIndexEntry> entries_;
};

} // namespace fastecu::definition
