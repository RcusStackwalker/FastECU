#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/memory/memory_image.h"
#include "src/algorithms/memory/memory_map.h"
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

    bool operator==(const RomProtocolInfo&) const = default;
};

struct SessionContents
{
    RomSource source;
    // The ROM file exactly as loaded, placed at ECU addresses by its memory
    // map (ADR 0020). Saving writes its file bytes back in this layout.
    memory::MemoryImage image;
    // nullopt: opened without a definition ("continue without definition
    // file"), a modeled state rather than placeholder rows.
    std::optional<ResolvedDefinition> definition;
    RomProtocolInfo protocol;
};

// One open ROM: the ROM file as loaded, placed at ECU addresses by its memory
// map (ADR 0020). Its bytes are the only truth for map values: there is no
// decoded-value cache, so a view decodes on demand and an edit writes bytes.
class CalibrationSession
{
  public:
    CalibrationSession(SessionId id, SessionContents contents);

    SessionId Id() const;
    const RomSource& Source() const;
    // The bytes as definitions address them: index i is the byte at the
    // definition base + i, up to the end of the highest memory block. Fill
    // blocks read as their fill byte and addresses no block holds as 0x00.
    // Derived from File() and the memory map; for most ROMs it equals File().
    bytes::ByteView Rom() const;
    // The ROM file as loaded, with every write since: what saving writes.
    bytes::ByteView File() const;
    const ResolvedDefinition *Definition() const;
    const RomProtocolInfo& Protocol() const;
    void SetProtocol(RomProtocolInfo protocol);
    // True once any write_bytes succeeded since the last save.
    bool Dirty() const;
    // Updates the saved path and basename, clears dirty, and preserves origin.
    void MarkSaved(std::string_view path);

    // Cells and axes of definition()->definition.maps[map_index], decoded from
    // the current bytes. InvalidConfig for an index past the last map, a
    // session without a definition, an unusable layout, or a map whose cells
    // or axes leave the memory map, touch a fill block, or span writable and
    // read-only memory (a structural map failure). Computation errors are
    // retained per numeric cell in the typed snapshot.
    Result<DecodedMap> DecodeMap(std::size_t map_index) const;

    // Whether WriteBytes(offset, ...) of `size` bytes would succeed: the whole
    // range lies in Rom() and in writable, ROM-file-backed memory.
    Status CheckWrite(std::uint64_t offset, std::size_t size) const;
    // The only mutation of the bytes, at definition address `offset`. A write
    // CheckWrite rejects changes nothing.
    Status WriteBytes(std::uint64_t offset, bytes::ByteView data);

  private:
    SessionId id_;
    RomSource source_;
    memory::MemoryImage image_;
    // Rom(): rendered from image_ at construction, then patched by WriteBytes.
    bytes::Bytes definition_view_;
    std::optional<ResolvedDefinition> definition_;
    RomProtocolInfo protocol_;
    bool dirty_{false};
};

} // namespace fastecu::calibration
