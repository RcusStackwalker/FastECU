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
    EXPECT_EQ(static_cast<int>(RomInfoRow::FlashMethod), 10);
    EXPECT_EQ(static_cast<int>(RomInfoRow::DefFile), 15);
    EXPECT_EQ(kRomInfoRowCount, 16);
}

TEST(RomInfo, DefinitionRowsUseProtocolMetadataAndDirectParent)
{
    definition::RomDefinition definition{.format = definition::DefinitionFormat::EcuFlash};
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
    auto protocol = session.protocol();
    protocol.flash_method = "updated";
    protocol.checksum_module = "updated checksum";
    session.set_protocol(protocol);
    EXPECT_EQ(rom_info_value(rom_info_values(session), RomInfoRow::FlashMethod), "updated");
    EXPECT_EQ(rom_info_value(rom_info_values(session), RomInfoRow::ChecksumModule), "updated checksum");
}

TEST(RomInfo, DefinitionlessAndContinueWithoutPlaceholders)
{
    const calibration::CalibrationSession session(calibration::SessionId{1},
                                                  {
                                                      .source = {.origin = calibration::RomOrigin::File},
                                                      .protocol = {.file_size_label = "16kb"},
                                                  });
    const auto plain = rom_info_values(session);
    EXPECT_EQ(plain.size(), kRomInfoRowCount);
    EXPECT_EQ(rom_info_value(plain, RomInfoRow::XmlId), " ");
    EXPECT_EQ(rom_info_value(plain, RomInfoRow::FlashMethod), " ");
    EXPECT_EQ(rom_info_value(plain, RomInfoRow::FileSize), "16kb");
    const auto continued = rom_info_values(session, QString("Subaru"));
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::XmlId), "UnknownID");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::InternalIdAddress), "");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::InternalIdString), "");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::EcuId), "");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::Make), "Subaru");
    EXPECT_EQ(rom_info_value(continued, RomInfoRow::DefFile), " ");
}

TEST(RomInfo, EcuReadAndHeaderOnlyDefinitionRetainMetadata)
{
    calibration::SessionContents contents{
        .source = {.origin = calibration::RomOrigin::EcuRead},
        .protocol = {.flash_method = "read protocol", .checksum_module = "No checksums", .file_size_label = "256kb"}};
    const calibration::CalibrationSession read(calibration::SessionId{1}, contents);
    EXPECT_EQ(rom_info_value(rom_info_values(read), RomInfoRow::FlashMethod), "read protocol");
    contents.definition = calibration::ResolvedDefinition{
        .definition = {.format = definition::DefinitionFormat::EcuFlash, .identity = {.xml_id = "HEADER"}}};
    contents.protocol.flash_method.clear();
    const calibration::CalibrationSession header(calibration::SessionId{2}, contents);
    EXPECT_EQ(rom_info_value(rom_info_values(header), RomInfoRow::XmlId), "HEADER");
    EXPECT_EQ(rom_info_value(rom_info_values(header), RomInfoRow::FlashMethod), "");
    EXPECT_EQ(rom_info_value(rom_info_values(header), RomInfoRow::FileSize), "256kb");
}
} // namespace
} // namespace fastecu::ui
