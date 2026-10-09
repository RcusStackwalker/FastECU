#include "src/ui/desktop/calibration/rom_info.h"

#include <gtest/gtest.h>

namespace fastecu::ui
{
namespace
{
TEST(RomInfo, PinsLabelsAndRowPositions)
{
    EXPECT_EQ(rom_info_labels(), (QStringList{"XML ID", "Internal ID Address", "Internal ID String", "ECU ID", "Make",
                                              "Market", "Model", "Submodel", "Transmission", "Year", "Flash Method",
                                              "Memory Model", "Checksum Module", "Rom Base", "File Size", "Def File"}));
    EXPECT_EQ(static_cast<int>(RomInfoRow::kFlashMethod), 10);
    EXPECT_EQ(static_cast<int>(RomInfoRow::kDefFile), 15);
    EXPECT_EQ(kRomInfoRowCount, 16);
}

TEST(RomInfo, DefinitionRowsUseProtocolMetadataAndDirectParent)
{
    definition::RomDefinition definition{.format = definition::DefinitionFormat::kEcuFlash};
    definition.identity = {.xml_id = "ID", .internal_id = "INT", .ecu_id = "ECU", .internal_id_address = 0x10};
    definition.metadata = {.make = "Subaru",
                           .market = "EU",
                           .model = "Impreza",
                           .submodel = "WRX",
                           .transmission = "MT",
                           .year = "2002",
                           .flash_method = "alias",
                           .memory_model = "FLASH",
                           .checksum_module = "definition checksum",
                           .file_size = "old"};
    definition.parents = {"PARENT"};
    definition.resolved_definition_ids = {"ROOT", "PARENT", "ID"};
    definition.source = "/defs/id.xml";
    calibration::CalibrationSession session(
        calibration::SessionId{1},
        {
            .definition = calibration::ResolvedDefinition{.definition = definition},
            .protocol = {.flash_method = "resolved", .checksum_module = "checksumresolved", .file_size_label = "128kb"},
        });
    EXPECT_EQ(rom_info_values(session),
              (QStringList{"ID", "10", "INT", "ECU", "Subaru", "EU", "Impreza", "WRX", "MT", "2002", "resolved",
                           "FLASH", "checksumresolved", "PARENT", "128kb", "/defs/id.xml"}));
    auto protocol = session.Protocol();
    protocol.flash_method = "updated";
    protocol.checksum_module = "updated checksum";
    session.SetProtocol(protocol);
    EXPECT_EQ(rom_info_value(rom_info_values(session), RomInfoRow::kFlashMethod), "updated");
    EXPECT_EQ(rom_info_value(rom_info_values(session), RomInfoRow::kChecksumModule), "updated checksum");
}

TEST(RomInfo, DefinitionlessAndContinueWithoutPlaceholders)
{
    const calibration::CalibrationSession session(calibration::SessionId{1},
                                                  {
                                                      .source = {.origin = calibration::RomOrigin::kFile},
                                                      .protocol = {.file_size_label = "16kb"},
                                                  });
    const auto plain = rom_info_values(session);
    EXPECT_EQ(plain.size(), kRomInfoRowCount);
    EXPECT_EQ(rom_info_value(plain, RomInfoRow::kXmlId), " ");
    EXPECT_EQ(rom_info_value(plain, RomInfoRow::kFlashMethod), " ");
    EXPECT_EQ(rom_info_value(plain, RomInfoRow::kFileSize), "16kb");
    const auto continued = rom_info_values(session, QString("Subaru"));
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::kXmlId), "UnknownID");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::kInternalIdAddress), "");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::kInternalIdString), "");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::kEcuId), "");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::kMake), "Subaru");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::kDefFile), " ");
}

TEST(RomInfo, EcuReadAndHeaderOnlyDefinitionRetainMetadata)
{
    calibration::SessionContents contents{
        .source = {.origin = calibration::RomOrigin::kEcuRead},
        .protocol = {.flash_method = "read protocol", .checksum_module = "No checksums", .file_size_label = "256kb"}};
    const calibration::CalibrationSession read(calibration::SessionId{1}, contents);
    EXPECT_EQ(rom_info_value(rom_info_values(read), RomInfoRow::kFlashMethod), "read protocol");
    contents.definition = calibration::ResolvedDefinition{
        .definition = {.format = definition::DefinitionFormat::kEcuFlash, .identity = {.xml_id = "HEADER"}}};
    contents.protocol.flash_method.clear();
    const calibration::CalibrationSession header(calibration::SessionId{2}, contents);
    EXPECT_EQ(rom_info_value(rom_info_values(header), RomInfoRow::kXmlId), "HEADER");
    EXPECT_EQ(rom_info_value(rom_info_values(header), RomInfoRow::kFlashMethod), "");
    EXPECT_EQ(rom_info_value(rom_info_values(header), RomInfoRow::kFileSize), "256kb");
}
} // namespace
} // namespace fastecu::ui
