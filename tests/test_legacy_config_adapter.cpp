#include "src/backend/config/legacy_config_adapter.h"
#include "src/backend/definitions/file_actions.h"
#include <gtest/gtest.h>
#include <map>

using fastecu::DirEntry;
using fastecu::ErrorKind;
using fastecu::IFileRepository;
using fastecu::IFileSystem;
using fastecu::IResourceBundle;
using fastecu::Result;
using fastecu::Status;
using fastecu::config::LegacyConfigAdapter;

namespace
{
class InMemoryFileSystem : public IFileSystem
{
  public:
    bool exists(std::string_view path) override
    {
        return dirs.count(std::string(path)) > 0;
    }
    Status create_directory(std::string_view path) override
    {
        dirs.insert(std::string(path));
        return {};
    }
    Status copy_file(std::string_view, std::string_view, bool) override
    {
        return {};
    }
    Status remove_file(std::string_view) override
    {
        return {};
    }
    Result<std::vector<DirEntry>> list_directory(std::string_view) override
    {
        return std::vector<DirEntry>{};
    }
    std::set<std::string> dirs;
};

class InMemoryResourceBundle : public IResourceBundle
{
  public:
    Result<std::vector<std::string>> list(std::string_view) override
    {
        return std::vector<std::string>{};
    }
    Result<std::vector<std::uint8_t>> read(std::string_view, std::string_view) override
    {
        return fastecu::fail(ErrorKind::InvalidConfig, "not used by this test");
    }
};

class InMemoryFileRepository : public IFileRepository
{
  public:
    Result<std::vector<std::uint8_t>> read(std::string_view handle) override
    {
        auto it = files.find(std::string(handle));
        if (it == files.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such handle");
        return it->second;
    }
    Status write(std::string_view handle, std::span<const std::uint8_t> data) override
    {
        files[std::string(handle)].assign(data.begin(), data.end());
        return {};
    }
    std::map<std::string, std::vector<std::uint8_t>> files;
};
} // namespace

TEST(LegacyConfigAdapterTest, SetBaseDirsPopulatesConfigValuesStructureAndReturnsSamePointer)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    LegacyConfigAdapter adapter(fs, bundle, repo);
    FileActions::ConfigValuesStructure values;

    fastecu::config::AppRootInfo root{"/base", true};
    auto *returned = adapter.set_base_dirs(&values, root);

    EXPECT_EQ(returned, &values);
    EXPECT_EQ(values.calibration_files_directory.toStdString(), "/base/calibrations/");
    EXPECT_EQ(values.config_file.toStdString(), "/base/config/fastecu.cfg");
}

TEST(LegacyConfigAdapterTest, ReadConfigFilePopulatesConfigValuesStructureFields)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    LegacyConfigAdapter adapter(fs, bundle, repo);
    FileActions::ConfigValuesStructure values;
    values.config_file = "fastecu.cfg";
    std::string xml =
        R"(<?xml version="1.0"?><config name="FastECU" version="x"><software_settings>)"
        R"(<setting name="serial_port"><value data="COM7"/></setting>)"
        R"(</software_settings></config>)";
    repo.files["fastecu.cfg"] = std::vector<std::uint8_t>(xml.begin(), xml.end());

    auto *returned = adapter.read_config_file(&values);

    ASSERT_NE(returned, nullptr);
    EXPECT_EQ(returned->serial_port.toStdString(), "COM7");
}

TEST(LegacyConfigAdapterTest, ReadProtocolsFilePopulatesParallelListsInLockstep)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    LegacyConfigAdapter adapter(fs, bundle, repo);
    FileActions::ConfigValuesStructure values;
    values.protocols_file = "protocols.cfg";
    std::string xml =
        R"(<?xml version="1.0"?><config name="FastECU" version="x"><protocols>)"
        R"(<protocol name="p1"><ecu>E1</ecu><mcu>M1</mcu></protocol>)"
        R"(</protocols></config>)";
    repo.files["protocols.cfg"] = std::vector<std::uint8_t>(xml.begin(), xml.end());

    auto *returned = adapter.read_protocols_file(&values);

    ASSERT_NE(returned, nullptr);
    ASSERT_EQ(returned->flash_protocol_protocol_name.size(), 1);
    EXPECT_EQ(returned->flash_protocol_protocol_name.at(0).toStdString(), "p1");
    EXPECT_EQ(returned->flash_protocol_ecu.at(0).toStdString(), "E1");
    EXPECT_EQ(returned->flash_protocol_mcu.at(0).toStdString(), "M1");
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
