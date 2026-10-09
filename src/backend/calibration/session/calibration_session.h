#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Identifies one open calibration within a workspace. Never reused, so a UI
// element still holding the ID of a closed session finds nothing rather than
// a different ROM.
enum class SessionId : std::uint64_t
{
};

enum class RomOrigin
{
    kFile,
    kEcuRead,
};

struct RomSource
{
    // Basename of `path`; "default.bin" when the path has none.
    std::string display_name;
    std::string path;
    RomOrigin origin{RomOrigin::kFile};

    bool operator==(const RomSource&) const = default;
};

struct ResolvedDefinition
{
    definition::DefinitionFormat format{definition::DefinitionFormat::kEcuFlash};
    std::string id;
    definition::RomDefinition definition;

    bool operator==(const ResolvedDefinition&) const = default;
};

// What the flash, checksum and ROM-info views need that is not part of the
// definition. Replaces the legacy FlashMethod/Kernel/KernelStartAddr/McuType/
// RomId fields and the protocol-derived RomInfo slots.
struct RomProtocolInfo
{
    std::string flash_method; // after alias resolution
    std::string checksum_module;
    std::string mcu_type;
    std::string kernel_path;
    std::string kernel_start_address;
    std::string rom_id;
    std::string file_size_label; // "<unpadded bytes / 1024>kb"
    std::size_t unpadded_size{0};

    bool operator==(const RomProtocolInfo&) const = default;
};

struct SessionContents
{
    RomSource source;
    std::vector<std::uint8_t> rom;
    // nullopt: opened without a definition ("continue without definition
    // file"), a modeled state rather than placeholder rows.
    std::optional<ResolvedDefinition> definition;
    RomProtocolInfo protocol;
};

// One open ROM. Its bytes are the only truth for map values: there is no
// decoded-value cache, so a view decodes on demand and an edit writes bytes.
class CalibrationSession
{
  public:
    CalibrationSession(SessionId id, SessionContents contents);

    SessionId Id() const;
    const RomSource& Source() const;
    bytes::ByteView Rom() const;
    const ResolvedDefinition *Definition() const;
    const RomProtocolInfo& Protocol() const;
    void SetProtocol(RomProtocolInfo protocol);
    // True once any write_bytes succeeded since the last save.
    bool Dirty() const;
    // Updates the saved path and basename, clears dirty, and preserves origin.
    void MarkSaved(std::string_view path);

    // Cells and axes of definition()->definition.maps[map_index], decoded from
    // the current bytes. InvalidConfig for an index past the last map or a
    // session without a definition or unusable layout. Computation errors are
    // retained per numeric cell in the typed snapshot.
    Result<DecodedMap> DecodeMap(std::size_t map_index) const;

    // The only mutation of the bytes. The whole of `data` must land inside the
    // image; a write that would not is rejected and changes nothing.
    Status WriteBytes(std::uint64_t offset, bytes::ByteView data);

  private:
    SessionId id_;
    SessionContents contents_;
    bool dirty_{false};
};

} // namespace fastecu::calibration
