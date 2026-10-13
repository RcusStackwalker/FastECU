#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/memory/testing/memory_views.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/calibration/session/testing/fake_definition_catalogs.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration::testing
{

inline constexpr std::size_t kBody = 0x10;

// A 3x2 uint8 body at 0x10 with identity scaling bounded to [0, 200], a
// three-element X axis at 0x00 and a two-element Y axis at 0x08.
inline definition::RomDefinition GridDefinition()
{
    definition::RomDefinition rom{.format = definition::DefinitionFormat::kEcuFlash};
    rom.scalings.push_back(definition::Scaling{.name = "raw",
                                               .from_byte = "x",
                                               .to_byte = "x",
                                               .minimum = "0",
                                               .maximum = "200",
                                               .coarse_increment = "10",
                                               .fine_increment = "1"});
    definition::CalibrationMap map;
    map.name = "Grid";
    map.type = "3D";
    map.address = memory::DefinitionAddress{kBody};
    map.x_size = 3;
    map.y_size = 2;
    map.storage_type = definition::StorageType::kUint8;
    map.endian = "big";
    map.scaling_name = "raw";
    map.x_axis.type = "X Axis";
    map.x_axis.address = memory::DefinitionAddress{0x00};
    map.x_axis.storage_type = definition::StorageType::kUint8;
    map.x_axis.scaling_name = "raw";
    map.y_axis.type = "Y Axis";
    map.y_axis.address = memory::DefinitionAddress{0x08};
    map.y_axis.storage_type = definition::StorageType::kUint8;
    map.y_axis.scaling_name = "raw";
    rom.maps.push_back(map);
    return rom;
}

// Body row-major: {10, 20, 30} / {40, 50, 200}; the last cell sits on the limit.
inline bytes::Bytes GridRom()
{
    bytes::Bytes rom(0x20, 0);
    rom[0] = 1;
    rom[1] = 2;
    rom[2] = 3;
    rom[8] = 4;
    rom[9] = 5;
    const std::array<std::uint8_t, 6> body{10, 20, 30, 40, 50, 200};
    std::ranges::copy(body, rom.begin() + static_cast<std::ptrdiff_t>(kBody));
    return rom;
}

class NumericSessionFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg_.Initialize(), fastecu::testing::IsOk());
        const auto opened = workspace_.AdoptReadImage(
            ReadImage{.rom = bytes::Bytes(8, 0), .filename = "grid.bin", .protocol_name = "proto_b"});
        ASSERT_THAT(opened, fastecu::testing::IsOk());
        id_ = opened->id;
        Install(GridDefinition());
    }

    // `rom` placed by `memory_map`, or by the identity map without one.
    static memory::MemoryImage PlaceGrid(bytes::Bytes rom, std::optional<memory::MemoryMap> memory_map)
    {
        return memory_map.has_value() ? memory::MemoryImage::Create(std::move(*memory_map), std::move(rom)).value()
                                      : memory::testing::IdentityImage(std::move(rom));
    }

    // Replaces the open session's contents with a clean session.
    void Install(std::optional<definition::RomDefinition> def, bytes::Bytes rom = GridRom(),
                 RomProtocolInfo protocol = {}, std::optional<memory::MemoryMap> memory_map = std::nullopt)
    {
        std::optional<ResolvedDefinition> resolved;
        if (def.has_value())
        {
            resolved = ResolvedDefinition{.definition = std::move(*def)};
        }
        *workspace_.Find(id_) =
            CalibrationSession(id_, SessionContents{.source = {.display_name = "grid.bin"},
                                                    .image = PlaceGrid(std::move(rom), std::move(memory_map)),
                                                    .definition = std::move(resolved),
                                                    .protocol = std::move(protocol)});
    }

    CalibrationSession& Session()
    {
        return *workspace_.Find(id_);
    }

    bytes::Bytes RomBytes()
    {
        const auto rom = Session().File();
        return {rom.begin(), rom.end()};
    }

    config::testing::ConfigSessionFixture cfg_;
    InMemoryAtomicFileWriter writer_;
    definition::DefinitionService definitions_{cfg_.file_system, cfg_.file_repository, writer_};
    testing::FakeDefinitionCatalogs catalogs_;
    RomOpenUseCase opener_{catalogs_, definitions_, cfg_.file_repository, cfg_.file_system, cfg_.events, cfg_.session};
    CalibrationWorkspace workspace_{opener_};
    SessionId id_{};
};

} // namespace fastecu::calibration::testing
