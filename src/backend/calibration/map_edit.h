#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Which of a map's three element runs an edit targets. Declared here rather
// than beside resolve_edit_target (Task 5) because the UI adapter (Task 4)
// needs it to choose which parallel-list group to pluck from, and that lands
// first.
enum class EditTargetKind
{
    MapBody,
    XAxis,
    YAxis,
    Rejected,
};

// One run of editable elements: a map's cells, or one axis's points. The
// non-owning counterpart of calibration_service.h's ElementRun, for the write
// side. string_view fields borrow from the EcuCalDefStructure lists the UI
// adapter reads them out of, which always outlive the call. Never store one.
struct MapElementSpec
{
    std::uint64_t address{0};
    std::optional<definition::StorageType> storage_type;
    std::string_view endian;
    std::string_view to_byte{"x"};
    std::string_view from_byte{"x"};
    // " " means "unset" in the legacy model, not "empty". Compared as text
    // before conversion, exactly as the legacy code does.
    std::string_view min_value{" "};
    std::string_view max_value{" "};
    double coarse_increment{0.0};
    double fine_increment{0.0};
    std::uint32_t x_size{1};
    std::uint32_t y_size{1};
    // The wrx02 relocation inputs. Carried as data rather than read from a
    // global so the rule is testable; see the spec's defect (a).
    std::string_view flash_method;
    std::uint64_t rom_file_size{0};
};

// Byte address of element `index`, including the wrx02 relocation. Exposed
// because the read and write paths apply *different* wrx02 predicates today
// and the difference must be visible and separately testable -- see the
// spec's defect (a). `for_write` selects set_rom_data_value's predicate;
// otherwise get_rom_data_value's is used.
std::uint64_t element_byte_address(const MapElementSpec& spec, std::uint32_t index, bool for_write);

// The raw stored value of element `index`, reproducing get_rom_data_value.
//
// Returns ErrorKind::Internal when the element's window would run past
// rom_data's end; legacy indexed QByteArray::at() unchecked here.
Result<std::int64_t> read_raw_element(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t index);

// Encodes `display_value` into `spec`'s on-ROM byte representation, reproducing
// the encode half shared verbatim by the three edit call sites in
// menu_actions.cpp (set_rom_data_value's callers). Runs `display_value`
// through spec.to_byte via expression_evaluate, using the same
// format_like_qt_g formatter decode_scaled_values uses, so the two cannot
// drift on how a double becomes the string an expression sees.
//
// Writes the byte order spec.endian's label claims -- this is the inverse of
// read_raw_element's *correct* half (the unsigned/float paths), not of its
// signed-multi-byte defect; see PinnedDefect_SignedMultiByteDoesNotRoundTrip*
// in map_edit_test.cpp for the resulting divergence on signed 16/32-bit
// storage.
Result<std::vector<std::uint8_t>> encode_scaled_value(const MapElementSpec& spec, double display_value,
                                                      int float_precision);

} // namespace fastecu::calibration
