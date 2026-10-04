#pragma once
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/config/catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/config/config_session.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_file_system.h"
#include "src/backend/ports/testing/in_memory_resource_bundle.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::config::testing
{

inline constexpr std::string_view kRoot = "/root";
inline constexpr std::string_view kVersion = "0.1.0-beta.5";

inline constexpr auto kStandardProtocols = std::to_array<ProtocolSpec>({
    {.name = "proto_a",
     .alias = "alias_a",
     .ecu = "ECU A",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::Corrected,
     .read = true,
     .test_write = false,
     .write = true,
     .flash_transport = "iso15765,CAN",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "a.bin",
     .kernel_load_address = 0xFFFF3000U,
     .description = "Protocol A"},
    {.name = "proto_b",
     .alias = "alias_b",
     .ecu = "ECU B",
     .mcu = "M32R",
     .mode = "OBD2",
     .checksum = ChecksumSupport::Missing,
     .read = true,
     .test_write = true,
     .write = true,
     .flash_transport = "K-Line",
     .log_transport = "K-Line",
     .log_protocol = "MUT_DMA",
     .kernel = "b.bin",
     .kernel_load_address = 0x0U,
     .description = "Protocol B"},
});

// Rows: 0 Subaru Impreza -> proto_a (shared with row 2), 1 Mitsubishi Colt ->
// proto_b, 2 Subaru Forester -> proto_a.
inline constexpr auto kStandardVehicles = std::to_array<VehicleSpec>({
    {.id = "subaru-impreza-v1",
     .make = "Subaru",
     .model = "Impreza",
     .version = "v1",
     .protocol = protocol_in(kStandardProtocols, "proto_a")},
    {.id = "mitsubishi-colt-v2",
     .make = "Mitsubishi",
     .model = "Colt",
     .version = "v2",
     .protocol = protocol_in(kStandardProtocols, "proto_b")},
    {.id = "subaru-forester-v3",
     .make = "Subaru",
     .model = "Forester",
     .version = "v3",
     .protocol = protocol_in(kStandardProtocols, "proto_a")},
});

inline constexpr Catalog kStandardCatalog{kStandardProtocols, kStandardVehicles};
static_assert(catalog_references_resolve(kStandardProtocols, kStandardVehicles));

inline std::string setting(std::string_view name, std::string_view data)
{
    return std::format(R"(<setting name="{}"><value data="{}"/></setting>)", name, data);
}

struct ConfigSessionFixture
{
    explicit ConfigSessionFixture(std::string_view root_path = kRoot)
        : root(root_path), paths(resolve_config_paths(root, kVersion))
    {
        put_settings("");
    }

    void put(const std::string& handle, std::string_view text)
    {
        file_repository.files[handle] = std::vector<std::uint8_t>(text.begin(), text.end());
    }
    std::string text(const std::string& handle) const
    {
        const std::vector<std::uint8_t>& bytes = file_repository.files.at(handle);
        return {bytes.begin(), bytes.end()};
    }
    void put_settings(std::string_view settings)
    {
        put(paths.config_file, std::format(R"(<?xml version="1.0"?><config name="FastECU" version="test">)"
                                           R"(<software_settings>{}</software_settings></config>)",
                                           settings));
    }
    Status initialize()
    {
        return session.initialize(root, kVersion);
    }

    std::string root;
    ConfigPaths paths;
    // The session reads this at initialize(); a test may assign another
    // catalog first.
    Catalog catalog = kStandardCatalog;
    InMemoryFileSystem file_system;
    InMemoryResourceBundle resource_bundle;
    InMemoryFileRepository file_repository;
    RecordingEventSink events;
    ConfigSession session{catalog, file_system, resource_bundle, file_repository, events};
};

} // namespace fastecu::config::testing
