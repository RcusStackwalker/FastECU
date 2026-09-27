#pragma once
#include <format>
#include <string>
#include <string_view>
#include <vector>

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

// Vehicle rows, in file order (row id == position):
//   0 Subaru Impreza    -> proto_a  (shared with row 2)
//   1 Mitsubishi Colt   -> proto_b
//   2 Subaru Forester   -> proto_a
//   3 Nissan Skyline    -> missing_proto (no <protocol> of that name)
inline constexpr std::string_view kStandardProtocols = R"(<?xml version="1.0"?>
<config name="FastECU" version="test">
  <protocols>
    <protocol name="proto_a" alias="alias_a">
      <ecu>ECU A</ecu><mcu>SH7058</mcu><mode>OBD2</mode><checksum>yes</checksum>
      <read>yes</read><test_write>no</test_write><write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport><log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol><kernel>a.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
      <description>Protocol A</description>
    </protocol>
    <protocol name="proto_b" alias="alias_b">
      <ecu>ECU B</ecu><mcu>M32R</mcu><mode>OBD2</mode><checksum>n/a</checksum>
      <read>yes</read><test_write>yes</test_write><write>yes</write>
      <flash_transport>K-Line</flash_transport><log_transport>K-Line</log_transport>
      <log_protocol>MUT_DMA</log_protocol><kernel>b.bin</kernel><kernel_addr>0x0</kernel_addr>
      <description>Protocol B</description>
    </protocol>
  </protocols>
  <car_models>
    <car_model><make>Subaru</make><model>Impreza</model><version>v1</version><protocol>proto_a</protocol></car_model>
    <car_model><make>Mitsubishi</make><model>Colt</model><version>v2</version><protocol>proto_b</protocol></car_model>
    <car_model><make>Subaru</make><model>Forester</model><version>v3</version><protocol>proto_a</protocol></car_model>
    <car_model><make>Nissan</make><model>Skyline</model><version>v4</version><protocol>missing_proto</protocol></car_model>
  </car_models>
</config>
)";

inline std::string setting(std::string_view name, std::string_view data)
{
    return std::format(R"(<setting name="{}"><value data="{}"/></setting>)", name, data);
}

struct ConfigSessionFixture
{
    explicit ConfigSessionFixture(std::string_view root_path = kRoot)
        : root(root_path), paths(resolve_config_paths(root, kVersion))
    {
        put_protocols(kStandardProtocols);
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
    void put_protocols(std::string_view document)
    {
        put(paths.protocols_file, document);
    }
    Status initialize()
    {
        return session.initialize(root, kVersion);
    }

    std::string root;
    ConfigPaths paths;
    InMemoryFileSystem file_system;
    InMemoryResourceBundle resource_bundle;
    InMemoryFileRepository file_repository;
    RecordingEventSink events;
    ConfigSession session{file_system, resource_bundle, file_repository, events};
};

} // namespace fastecu::config::testing
