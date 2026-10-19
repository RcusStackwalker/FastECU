#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/element_run.h"
#include "src/backend/calibration/map_edit.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration::internal
{
class MapElement;
MapElement ResolveMapElement(const definition::RomDefinition& definition, const definition::CalibrationMap& map,
                             NumericTarget target);

// An operation-local owning snapshot of one map body or axis. Build the
// borrowed views only after the last move, and keep this owner alive until
// their consumers finish. The numeric metadata stays private: decoding reads it
// through Run(), editing through EditSpec().
class MapElement
{
  public:
    std::optional<memory::DefinitionAddress> address;
    std::optional<definition::StorageType> storage_type;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t start_position{1};
    std::uint32_t interval{1};
    // A bloblist's width is its first selection's length.
    std::uint32_t byte_width{1};

    std::uint64_t Count() const
    {
        return std::uint64_t{width} * height;
    }

    ElementRun Run() const&;
    MapElementSpec EditSpec() const&;
    ElementRun Run() const&& = delete;
    MapElementSpec EditSpec() const&& = delete;

  private:
    friend MapElement ResolveMapElement(const definition::RomDefinition&, const definition::CalibrationMap&,
                                        NumericTarget);

    std::string endian_;
    std::string from_byte_{"x"};
    std::string to_byte_{"x"};
    std::string min_value_;
    std::string max_value_;
    double coarse_increment_{0.0};
    double fine_increment_{0.0};
    // The map's own size, which differs from width and height for an axis.
    MapDimensions dimensions_;
    bool is_selectable_{false};
};

// The bytes choosing a named selection writes into a selectable map's body.
struct BlobPatch
{
    std::uint64_t address{0};
    bytes::Bytes bytes;
};

enum class BlobMiss
{
    // Not a bloblist body with at least one selection to choose from.
    kNotABlob,
    // No selection carries the requested name.
    kUnknownSelection,
};

// A selection whose width differs from the blob width is an InvalidConfig
// error. A map without an address resolves to address 0.
Result<std::variant<BlobPatch, BlobMiss>> ResolveBlobSelection(const definition::RomDefinition& definition,
                                                               const definition::CalibrationMap& map,
                                                               std::string_view selection);

} // namespace fastecu::calibration::internal
