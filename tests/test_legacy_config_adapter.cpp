#include "src/backend/config/legacy_config_adapter.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_file_system.h"
#include "src/backend/ports/testing/in_memory_resource_bundle.h"
#include <gtest/gtest.h>
#include <map>

using fastecu::DirEntry;
using fastecu::ErrorKind;
using fastecu::InMemoryFileRepository;
using fastecu::InMemoryFileSystem;
using fastecu::InMemoryResourceBundle;
using fastecu::Result;
using fastecu::Status;
using fastecu::config::LegacyConfigAdapter;

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

// Legacy read_config_file called save_config_file(configValues) on the same
// shared struct as its last step, so save's trailing-slash normalization
// landed in the in-memory struct immediately -- callers saw the normalized
// path right after read_config_file returned, not only on a later read.
// load_app_config deliberately returns the pre-save (unnormalized) parse,
// so LegacyConfigAdapter::read_config_file must save again and copy that
// normalized result into `values`, not the raw parse.
TEST(LegacyConfigAdapterTest, ReadConfigFileNormalizesTrailingSlashInMemory)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    LegacyConfigAdapter adapter(fs, bundle, repo);
    FileActions::ConfigValuesStructure values;
    values.config_file = "fastecu.cfg";
    std::string xml =
        R"(<?xml version="1.0"?><config name="FastECU" version="x"><software_settings>)"
        R"(<setting name="calibration_files_directory"><value data="/cal/no/trailing/slash"/></setting>)"
        R"(</software_settings></config>)";
    repo.files["fastecu.cfg"] = std::vector<std::uint8_t>(xml.begin(), xml.end());

    auto *returned = adapter.read_config_file(&values);

    ASSERT_NE(returned, nullptr);
    EXPECT_EQ(returned->calibration_files_directory.toStdString(), "/cal/no/trailing/slash/");
}

// Legacy read_protocols_file (file_actions.cpp:1089-1393, confirmed via
// `git show c967bec:src/backend/definitions/file_actions.cpp`) parses
// <protocols> into *local-only* variables, never `configValues->
// flash_protocol_*` -- those lists are populated exclusively by the
// <car_models> loop, one row per <car_model>, cross-referencing the local
// protocol lookup table by name. A protocols-only fixture with no
// <car_models> therefore leaves every flash_protocol_* list empty, not
// populated with one row per <protocol> (see
// FileActions::validate_flash_protocols, which validates every
// flash_protocol_* list's length against flash_protocol_id.size() --
// flash_protocol_id is never touched by the protocols section at all -- and
// MainWindow/protocol_select.cpp/vehicle_select.cpp, which all iterate
// flash_protocol_id.length() as the shared row count).
TEST(LegacyConfigAdapterTest, ReadProtocolsFileWithNoCarModelsLeavesListsEmpty)
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
    EXPECT_EQ(returned->flash_protocol_id.size(), 0);
    EXPECT_EQ(returned->flash_protocol_protocol_name.size(), 0);
    EXPECT_EQ(returned->flash_protocol_ecu.size(), 0);
}

// The critical whole-branch-review fix: <car_models> is a sibling section of
// <protocols>, and each <car_model>'s own fields plus its cross-referenced
// protocol fields land in configValues->flash_protocol_* as one row.
TEST(LegacyConfigAdapterTest, ReadProtocolsFileJoinsCarModelWithMatchingProtocol)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    LegacyConfigAdapter adapter(fs, bundle, repo);
    FileActions::ConfigValuesStructure values;
    values.protocols_file = "protocols.cfg";
    std::string xml =
        R"(<?xml version="1.0"?><config name="FastECU" version="x">)"
        R"(<protocols>)"
        R"(<protocol name="p1" alias="P One"><ecu>E1</ecu><mcu>M1</mcu><mode>OBD2</mode>)"
        R"(<checksum>yes</checksum><read>yes</read><test_write>no</test_write><write>yes</write>)"
        R"(<flash_transport>CAN</flash_transport><log_transport>K-Line</log_transport>)"
        R"(<log_protocol>SSM</log_protocol><description>Protocol One</description></protocol>)"
        R"(</protocols>)"
        R"(<car_models>)"
        R"(<car_model><make>Mitsubishi</make><model>Colt</model><version>Ralliart</version>)"
        R"(<type>4G93T</type><kw>110</kw><hp>150</hp><fuel>Petrol</fuel><year>2005</year>)"
        R"(<protocol>p1</protocol></car_model>)"
        R"(</car_models>)"
        R"(</config>)";
    repo.files["protocols.cfg"] = std::vector<std::uint8_t>(xml.begin(), xml.end());

    auto *returned = adapter.read_protocols_file(&values);

    ASSERT_NE(returned, nullptr);
    // Exactly one row -- protocols.size() (1) + car_models.size() (1) would
    // be 2, but only car_models produce rows in configValues.
    ASSERT_EQ(returned->flash_protocol_id.size(), 1);
    EXPECT_EQ(returned->flash_protocol_id.at(0).toStdString(), "0");
    EXPECT_EQ(returned->flash_protocol_make.at(0).toStdString(), "Mitsubishi");
    EXPECT_EQ(returned->flash_protocol_model.at(0).toStdString(), "Colt");
    EXPECT_EQ(returned->flash_protocol_version.at(0).toStdString(), "Ralliart");
    EXPECT_EQ(returned->flash_protocol_type.at(0).toStdString(), "4G93T");
    EXPECT_EQ(returned->flash_protocol_kw.at(0).toStdString(), "110");
    EXPECT_EQ(returned->flash_protocol_hp.at(0).toStdString(), "150");
    EXPECT_EQ(returned->flash_protocol_fuel.at(0).toStdString(), "Petrol");
    EXPECT_EQ(returned->flash_protocol_year.at(0).toStdString(), "2005");
    EXPECT_EQ(returned->flash_protocol_protocol_name.at(0).toStdString(), "p1");
    // Cross-referenced from the matched ProtocolEntry.
    EXPECT_EQ(returned->flash_protocol_alias.at(0).toStdString(), "P One");
    EXPECT_EQ(returned->flash_protocol_ecu.at(0).toStdString(), "E1");
    EXPECT_EQ(returned->flash_protocol_mcu.at(0).toStdString(), "M1");
    EXPECT_EQ(returned->flash_protocol_mode.at(0).toStdString(), "OBD2");
    EXPECT_EQ(returned->flash_protocol_checksum.at(0).toStdString(), "yes");
    EXPECT_EQ(returned->flash_protocol_read.at(0).toStdString(), "yes");
    EXPECT_EQ(returned->flash_protocol_test_write.at(0).toStdString(), "no");
    EXPECT_EQ(returned->flash_protocol_write.at(0).toStdString(), "yes");
    EXPECT_EQ(returned->flash_protocol_flash_transport.at(0).toStdString(), "CAN");
    EXPECT_EQ(returned->flash_protocol_log_transport.at(0).toStdString(), "K-Line");
    EXPECT_EQ(returned->flash_protocol_log_protocol.at(0).toStdString(), "SSM");
    EXPECT_EQ(returned->flash_protocol_description.at(0).toStdString(), "Protocol One");

    // validate_flash_protocols must accept this shape (every list the same
    // length as flash_protocol_id, and flash_protocol_id/protocol_name
    // populated) -- if the join were wrong (e.g. length mismatch) this
    // would fail, same as it would inside real read_protocols_file.
    QStringList validationErrors;
    EXPECT_TRUE(FileActions::validate_flash_protocols(*returned, &validationErrors)) << validationErrors.join(", ").toStdString();
}

// A car_model whose <protocol> text doesn't match any real protocol name
// (this happens in the real shipped protocols.cfg today --
// "sub_ecu_unisia_jecs_92"/"sub_ecu_unisia_jecs_97" reference protocols
// absent from its <protocols> section) must get legacy's " " placeholder in
// every cross-referenced field, not an out-of-bounds read or a crash.
TEST(LegacyConfigAdapterTest, ReadProtocolsFileUnmatchedCarModelGetsPlaceholders)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    InMemoryFileRepository repo;
    LegacyConfigAdapter adapter(fs, bundle, repo);
    FileActions::ConfigValuesStructure values;
    values.protocols_file = "protocols.cfg";
    std::string xml =
        R"(<?xml version="1.0"?><config name="FastECU" version="x">)"
        R"(<protocols>)"
        R"(<protocol name="p1"><ecu>E1</ecu></protocol>)"
        R"(</protocols>)"
        R"(<car_models>)"
        R"(<car_model><make>Subaru</make><model>Legacy</model>)"
        R"(<protocol>does_not_exist</protocol></car_model>)"
        R"(</car_models>)"
        R"(</config>)";
    repo.files["protocols.cfg"] = std::vector<std::uint8_t>(xml.begin(), xml.end());

    auto *returned = adapter.read_protocols_file(&values);

    ASSERT_NE(returned, nullptr);
    ASSERT_EQ(returned->flash_protocol_id.size(), 1);
    EXPECT_EQ(returned->flash_protocol_make.at(0).toStdString(), "Subaru");
    EXPECT_EQ(returned->flash_protocol_protocol_name.at(0).toStdString(), "does_not_exist");
    EXPECT_EQ(returned->flash_protocol_alias.at(0).toStdString(), " ");
    EXPECT_EQ(returned->flash_protocol_ecu.at(0).toStdString(), " ");
    EXPECT_EQ(returned->flash_protocol_mcu.at(0).toStdString(), " ");
    EXPECT_EQ(returned->flash_protocol_description.at(0).toStdString(), " ");
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
