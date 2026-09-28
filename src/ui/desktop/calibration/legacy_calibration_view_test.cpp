#include "src/ui/desktop/calibration/legacy_calibration_view.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::ui
{
namespace
{

using fastecu::testing::IsOk;

// "TESTROM" at 0x10, one uint8 map byte 0x2A at 0x20; synthetic.
std::vector<std::uint8_t> synthetic_rom(std::size_t size = 64)
{
    std::vector<std::uint8_t> rom(std::max<std::size_t>(size, 0x21), 0);
    const std::string id = "TESTROM";
    std::ranges::copy(id, rom.begin() + 0x10);
    rom[0x20] = 0x2A;
    rom.resize(size); // a short image drops the map byte, as the size-rejection case needs
    return rom;
}

constexpr std::string_view kDefinition = R"xml(
<rom>
  <romid><xmlid>TESTROM</xmlid><internalidaddress>10</internalidaddress>
    <internalidstring>TESTROM</internalidstring><make>Subaru</make>
    <flashmethod>alias_a</flashmethod><checksummodule>def-module</checksummodule></romid>
  <scaling name="Raw" toexpr="x" frexpr="x" format="%d" storagetype="uint8" endian="big"/>
  <table name="Idle" address="20" type="1D" category="Idle" scaling="Raw" storagetype="uint8"/>
</rom>)xml";

class LegacyCalibrationView : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg.initialize(), IsOk());
    }

    void enable_ecuflash_definitions()
    {
        cfg.put("/defs/test.xml", kDefinition);
        cfg.file_system.files["/defs/test.xml"] = {};
        cfg.file_system.directory_entries["/defs"] = {{.name = "test.xml", .is_directory = false}};
        auto& settings = cfg.session.settings();
        settings.primary_definition_base = "ecuflash";
        settings.use_ecuflash_definitions = "enabled";
        settings.ecuflash_definition_files_directory = "/defs";
        file_actions.create_ecuflash_def_id_list();
    }

    // What MainWindow::open_calibration_file left in its slot.
    FileActions::EcuCalDefStructure legacy_open(const QString& path)
    {
        FileActions::EcuCalDefStructure slot;
        while (slot.RomInfo.length() < slot.RomInfoStrings.length())
        {
            slot.RomInfo.append(" ");
        }
        EXPECT_NE(file_actions.open_subaru_rom_file(&slot, path), nullptr);
        return slot;
    }

    FileActions::EcuCalDefStructure projected(const calibration::RomOpenOutcome& outcome)
    {
        const calibration::CalibrationSession session(calibration::SessionId{1}, outcome.contents);
        auto view = project_legacy_calibration(session);
        EXPECT_THAT(view, IsOk());
        EXPECT_FALSE(view->decode_error.has_value());
        return *view->view;
    }

    config::testing::ConfigSessionFixture cfg;
    InMemoryAtomicFileWriter writer;
    FileActions file_actions{cfg.file_system, cfg.resource_bundle, cfg.file_repository,
                             writer,          cfg.events,          cfg.session};
    definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    calibration::RomOpenUseCase opener{file_actions,    definitions, cfg.file_repository,
                                       cfg.file_system, cfg.events,  cfg.session};
};

TEST_F(LegacyCalibrationView, FileWithoutDefinitionMatchesLegacyOpen)
{
    cfg.file_repository.files["/cal/a.bin"] = synthetic_rom();

    const FileActions::EcuCalDefStructure legacy = legacy_open("/cal/a.bin");
    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto outcome = opener.open_file("/cal/a.bin");
    ASSERT_THAT(outcome, IsOk());

    EXPECT_EQ(projected(*outcome), legacy);
}

TEST_F(LegacyCalibrationView, EcuFlashDefinitionMatchesLegacyOpen)
{
    enable_ecuflash_definitions();
    cfg.file_repository.files["/cal/a.bin"] = synthetic_rom();

    const FileActions::EcuCalDefStructure legacy = legacy_open("/cal/a.bin");
    ASSERT_TRUE(legacy.use_ecuflash_definition);
    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto outcome = opener.open_file("/cal/a.bin");
    ASSERT_THAT(outcome, IsOk());

    const FileActions::EcuCalDefStructure view = projected(*outcome);
    EXPECT_EQ(view, legacy);
    EXPECT_EQ(view.MapData, QStringList{"42,"});
    EXPECT_EQ(view.RomInfo.at(FileActions::InternalIdAddress), QString("10")); // 0x stripped
}

TEST_F(LegacyCalibrationView, EcuReadMatchesLegacyHandoff)
{
    enable_ecuflash_definitions();
    const std::string filename = "TESTROM2026-09-28_10h00m00s.bin";

    // MainWindow's read branch: pre-filled slot, protocol fields, then open.
    FileActions::EcuCalDefStructure legacy;
    while (legacy.RomInfo.length() < legacy.RomInfoStrings.length())
    {
        legacy.RomInfo.append(" ");
    }
    legacy.RomInfo.replace(FileActions::FlashMethod, "proto_b");
    legacy.FlashMethod = "proto_b";
    legacy.Kernel = "/kernels/b.bin";
    legacy.KernelStartAddr = "0x0";
    legacy.McuType = "M32R";
    const std::vector<std::uint8_t> rom = synthetic_rom();
    legacy.FullRomData = QByteArray(reinterpret_cast<const char *>(rom.data()), static_cast<qsizetype>(rom.size()));
    legacy.RomId = "TESTROM";
    legacy.FileName = QString::fromStdString(filename);
    ASSERT_NE(file_actions.open_subaru_rom_file(&legacy, legacy.FileName), nullptr);

    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto outcome = opener.adopt_read_image(calibration::ReadImage{
        .rom = rom,
        .filename = filename,
        .rom_id = "TESTROM",
        .protocol_name = "proto_b",
        .kernel_path = "/kernels/b.bin",
        .kernel_start_address = "0x0",
    });
    ASSERT_THAT(outcome, IsOk());

    FileActions::EcuCalDefStructure view = projected(*outcome);
    // The FlashMethod *field* is write-only in the UI: legacy kept the
    // protocol selected before the read, the session keeps the resolved one.
    view.FlashMethod = legacy.FlashMethod;
    EXPECT_EQ(view, legacy);
}

TEST_F(LegacyCalibrationView, SizeRejectedDefinitionShowsNoMaps)
{
    enable_ecuflash_definitions();
    cfg.file_repository.files["/cal/short.bin"] = synthetic_rom(0x20);

    const FileActions::EcuCalDefStructure legacy = legacy_open("/cal/short.bin");
    ASSERT_THAT(cfg.session.select_row(0), IsOk());
    const auto outcome = opener.open_file("/cal/short.bin");
    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->size_rejected);

    const FileActions::EcuCalDefStructure view = projected(*outcome);
    // Legacy cleared only NameList; every consumer iterates NameList, so an
    // empty map set is the same presentation.
    EXPECT_TRUE(legacy.NameList.isEmpty());
    EXPECT_TRUE(view.NameList.isEmpty());
    EXPECT_EQ(view.RomInfo, legacy.RomInfo);
    EXPECT_EQ(view.FullRomData, legacy.FullRomData);
    EXPECT_EQ(view.use_ecuflash_definition, legacy.use_ecuflash_definition);
}

} // namespace
} // namespace fastecu::ui
