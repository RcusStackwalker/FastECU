#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/memory/memory_map.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/rom_save.h"
#include "src/backend/checksum/checksum_selection.h"
#include "src/backend/config/catalog.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/ui/desktop/calibration/calibration_interaction.h"
#include "src/ui/desktop/calibration/testing/mock_calibration_interaction.h"
#include "src/ui/desktop/checksum/checksum_correction_result.h"

namespace
{

namespace calibration = fastecu::calibration;
namespace checksum = fastecu::checksum;
namespace config = fastecu::config;
namespace memory = fastecu::memory;
namespace ui = fastecu::ui;
using fastecu::Error;
using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::testing::IsOk;
using ::testing::_;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Pair;
using ::testing::Return;
using ::testing::SizeIs;
using ::testing::StartsWith;
using ::testing::StrictMock;
using ui::CalibrationNotice;
using ui::ChecksumCorrectionResult;
using ui::MockCalibrationInteraction;
using ui::PreparedWrite;
using ui::SaveMode;
using ui::SaveOutcome;

using LogLine = std::pair<LogLevel, std::string>;

// What the session looked like when the description callback fired, so a case
// can pin where the callback sits in the refresh.
struct DescriptionEvent
{
    std::string description;
    std::size_t logs_before;
    calibration::RomProtocolInfo session_protocol;
};

calibration::ResolvedDefinition headerOnlyDefinition()
{
    return calibration::ResolvedDefinition{.id = "HEADER"};
}

// An ECU read of {1, 2, 3}; open_session() dirties it to {9, 2, 3}.
calibration::CalibrationSession ecuRead(std::optional<calibration::ResolvedDefinition> definition,
                                        std::string flashMethod)
{
    return calibration::CalibrationSession{
        calibration::SessionId{1},
        calibration::SessionContents{
            .source = {.display_name = "read.bin", .path = "/old/read.bin", .origin = calibration::RomOrigin::kEcuRead},
            .rom = {1, 2, 3},
            .definition = std::move(definition),
            .protocol = {.flash_method = std::move(flashMethod), .rom_id = "TEST"},
        }};
}

// An MC68HC16Y5-style packed file of {1, 2, 3, 4}: file 0-1 at 0, two fill
// bytes at 2, file 2-3 at 4, so Rom() is {1, 2, 0xFF, 0xFF, 3, 4}.
calibration::CalibrationSession packedFile()
{
    const auto range = [](std::uint32_t start, std::uint32_t size)
    {
        return memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size})
            .value();
    };
    const std::array blocks{
        memory::MemoryBlock{
            .range = range(0, 2), .backing = memory::FileBacking{}, .writability = memory::Writability::kWritable},
        memory::MemoryBlock{
            .range = range(2, 2), .backing = memory::FillBacking{}, .writability = memory::Writability::kReadOnly},
        memory::MemoryBlock{.range = range(4, 2),
                            .backing = memory::FileBacking{.offset = memory::FileOffset{2}},
                            .writability = memory::Writability::kWritable},
    };
    return calibration::CalibrationSession{
        calibration::SessionId{2}, calibration::SessionContents{
                                       .source = {.display_name = "packed.bin",
                                                  .path = "/cal/packed.bin",
                                                  .origin = calibration::RomOrigin::kFile},
                                       .rom = {1, 2, 3, 4},
                                       .memory_map = memory::MemoryMap::Create(blocks, memory::ByteCount{4}).value(),
                                       .protocol = {.rom_id = "TEST"},
                                   }};
}

// A coordinator over an initialized in-memory configuration, the real
// RomSaveUseCase, and a strict interaction mock, so any unscripted dialog
// fails the case. Callbacks record owned copies of what they receive.
template <typename TestBase> class CoordinatorHarness : public TestBase
{
  protected:
    void SetUp() override
    {
        ASSERT_NO_FATAL_FAILURE(start(config::testing::kStandardCatalog));
        ASSERT_NO_FATAL_FAILURE(openSession(std::nullopt, {}));
    }

    // Initializes the configuration over `catalog`, selects row 0, and drops
    // what initialization recorded so a case sees only its own operation.
    void start(const config::Catalog& catalog)
    {
        cfg_.catalog = catalog;
        ASSERT_THAT(cfg_.Initialize(), IsOk());
        ASSERT_THAT(cfg_.session.SelectRow(0), IsOk());
        cfg_.file_repository.read_handles.clear();
        cfg_.file_repository.write_calls.clear();
        cfg_.events.logs.clear();
        cfg_.events.notices.clear();
    }

    void openSession(std::optional<calibration::ResolvedDefinition> definition, std::string flashMethod)
    {
        session_ = ecuRead(std::move(definition), std::move(flashMethod));
        ASSERT_THAT(session_.WriteBytes(0, bytes::Bytes{9}), IsOk());
    }

    // Scripts one checksum interaction returning `result`, recording its
    // arguments and its place in the callback trace.
    void expectChecksum(ChecksumCorrectionResult result)
    {
        EXPECT_CALL(interaction_, correctChecksums(_, _, _))
            .WillOnce(
                [this, result](bytes::ByteView image, bool hasDefinition, const checksum::ChecksumSelection& selection)
                {
                    checksum_image_.assign(image.begin(), image.end());
                    checksum_has_definition_ = hasDefinition;
                    checksum_selection_ = selection;
                    trace_.emplace_back("checksum");
                    return result;
                });
    }

    // Scripts one Save As picker returning `chosen`, recording the suggested
    // path, how many logs preceded it, and its place in the callback trace.
    // Nothing may reach the repository before a destination is chosen.
    void expectChoose(std::optional<std::string> chosen)
    {
        EXPECT_CALL(interaction_, chooseSavePath(_))
            .WillOnce(
                [this, chosen](std::string_view suggested)
                {
                    EXPECT_THAT(cfg_.file_repository.write_calls, IsEmpty());
                    suggested_path_ = suggested;
                    logs_before_choose_ = logs_.size();
                    trace_.emplace_back("choose");
                    return chosen;
                });
    }

    // Where a save in `mode` persists: the session's own path for Save, or
    // `chosen` from a scripted picker for Save As.
    std::string expectDestination(SaveMode mode, const std::string& chosen = "/cal/saved.bin")
    {
        if (mode == SaveMode::kSave)
        {
            return session_.Source().path;
        }
        expectChoose(chosen);
        return chosen;
    }

    std::vector<LogLine> logs_;
    std::vector<DescriptionEvent> descriptions_;
    std::vector<std::string> trace_;
    bytes::Bytes checksum_image_;
    bool checksum_has_definition_ = false;
    checksum::ChecksumSelection checksum_selection_;
    std::string suggested_path_;
    std::size_t logs_before_choose_ = 0;

    config::testing::ConfigSessionFixture cfg_;
    calibration::RomSaveUseCase saver_{cfg_.file_repository, cfg_.events};
    StrictMock<MockCalibrationInteraction> interaction_;
    calibration::CalibrationSession session_ = ecuRead(std::nullopt, {});
    ui::CalibrationOperationCoordinator coordinator_{
        cfg_.session, saver_, interaction_,
        ui::CalibrationPresentationCallbacks{
            .log = [this](LogLevel level, std::string_view text) { logs_.emplace_back(level, text); },
            .protocol_description_changed =
                [this](std::string_view description)
            {
                descriptions_.push_back({std::string{description}, logs_.size(), session_.Protocol()});
                trace_.push_back(std::format("protocol:{}", description));
            },
        }};
};

class CalibrationOperationCoordinator : public CoordinatorHarness<::testing::Test>
{
};

TEST_F(CalibrationOperationCoordinator, MissingWriteSelection)
{
    EXPECT_CALL(interaction_, showNotice(CalibrationNotice::kNoCalibrationToWrite));

    EXPECT_EQ(coordinator_.prepareWrite(nullptr, "/kernels/"), std::nullopt);

    EXPECT_THAT(logs_, IsEmpty());
    EXPECT_THAT(descriptions_, IsEmpty());
}

TEST_F(CalibrationOperationCoordinator, CancelledWriteWarning)
{
    ASSERT_THAT(cfg_.session.SelectRow(1), IsOk()); // proto_b: checksum n/a
    const calibration::RomProtocolInfo protocolBefore = session_.Protocol();
    EXPECT_CALL(interaction_, confirmWriteWithoutChecksum()).WillOnce(Return(false));

    EXPECT_EQ(coordinator_.prepareWrite(&session_, "/kernels/"), std::nullopt);

    // The refresh would have filled the MCU and kernel fields.
    EXPECT_EQ(session_.Protocol(), protocolBefore);
    EXPECT_THAT(logs_, Contains(Pair(LogLevel::kDebug, "Write canceled!")));
    EXPECT_THAT(descriptions_, IsEmpty());
}

TEST_F(CalibrationOperationCoordinator, AcceptedWriteWarningSkipsCorrection)
{
    ASSERT_THAT(cfg_.session.SelectRow(1), IsOk()); // proto_b: checksum n/a
    EXPECT_CALL(interaction_, confirmWriteWithoutChecksum()).WillOnce(Return(true));

    const std::optional<PreparedWrite> prepared = coordinator_.prepareWrite(&session_, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    EXPECT_THAT(prepared->image, ElementsAre(9, 2, 3));
    EXPECT_EQ(prepared->protocol.name, "proto_b");
    EXPECT_EQ(prepared->protocol.mcu, "M32R");
    EXPECT_EQ(prepared->kernel_path, "/kernels/b.bin");
    EXPECT_EQ(prepared->display_filename, "read.bin");
    EXPECT_EQ(session_.Protocol().mcu_type, "M32R");
    EXPECT_THAT(logs_, Not(Contains(Pair(LogLevel::kDebug, "Write canceled!"))));
}

TEST_F(CalibrationOperationCoordinator, CorrectedWriteUsesOnlyOperationBytes)
{
    expectChecksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});

    const std::optional<PreparedWrite> prepared = coordinator_.prepareWrite(&session_, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    EXPECT_THAT(checksum_image_, ElementsAre(9, 2, 3));
    EXPECT_FALSE(checksum_has_definition_);
    EXPECT_THAT(prepared->image, ElementsAre(4, 5, 6));
    EXPECT_THAT(session_.Rom(), ElementsAre(9, 2, 3));
    EXPECT_TRUE(session_.Dirty());
    EXPECT_EQ(prepared->protocol.name, "proto_a");
    EXPECT_EQ(prepared->protocol.mcu, "SH7058");
    EXPECT_EQ(prepared->kernel_path, "/kernels/a.bin");
    EXPECT_EQ(prepared->display_filename, "read.bin");
    EXPECT_THAT(cfg_.file_repository.write_calls, IsEmpty());
    EXPECT_THAT(cfg_.events.notices, IsEmpty());
}

TEST_F(CalibrationOperationCoordinator, ChecksumLogsKeepLegacyTextAndOrder)
{
    // 0x1a bytes, so the size line shows the lowercase hexadecimal legacy
    // QString::number(n, 16) produced.
    session_ = calibration::CalibrationSession{calibration::SessionId{2},
                                               calibration::SessionContents{.rom = bytes::Bytes(0x1a, 0)}};
    expectChecksum({.canceled_due_to_missing_module = true});

    ASSERT_TRUE(coordinator_.prepareWrite(&session_, "/kernels/").has_value());

    EXPECT_THAT(logs_, ElementsAre(Pair(LogLevel::kDebug, "Protocol: proto_a"), Pair(LogLevel::kDebug, "Make: Subaru"),
                                   Pair(LogLevel::kDebug, "Checksum: yes"),
                                   Pair(LogLevel::kDebug, "ecuCalDef->McuType: SH7058 SH7058"),
                                   Pair(LogLevel::kDebug, "Size: 0x1a -> 0x100000"),
                                   Pair(LogLevel::kDebug, "Checksum calculation canceled!")));
}

// The standard catalog with row 2 made a Nissan.
constexpr auto kNissanForesterVehicles = std::to_array<config::VehicleSpec>({
    config::testing::kStandardVehicles[0],
    config::testing::kStandardVehicles[1],
    {.id = "nissan-forester-v3",
     .make = "Nissan",
     .model = "Forester",
     .version = "v3",
     .protocol = config::ProtocolIn(config::testing::kStandardProtocols, "proto_a")},
});
constexpr config::Catalog kNissanForesterCatalog{config::testing::kStandardProtocols, kNissanForesterVehicles};

TEST_F(CalibrationOperationCoordinator, EmptyDefinedMethodReselectsBeforeChecksum)
{
    // Row 2 is the last proto_a row; making it a Nissan shows that the
    // checksum request reads the reselected vehicle, not row 0.
    ASSERT_NO_FATAL_FAILURE(start(kNissanForesterCatalog));
    ASSERT_NO_FATAL_FAILURE(openSession(headerOnlyDefinition(), {}));
    expectChecksum({});

    const std::optional<PreparedWrite> prepared = coordinator_.prepareWrite(&session_, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    ASSERT_THAT(cfg_.session.SelectedRow(), IsOk());
    EXPECT_EQ(*cfg_.session.SelectedRow(), 2U);
    EXPECT_EQ(session_.Protocol().flash_method, "proto_a");
    EXPECT_EQ(session_.Protocol().mcu_type, "SH7058");
    EXPECT_EQ(session_.Protocol().kernel_path, "/kernels/a.bin");
    EXPECT_EQ(session_.Protocol().kernel_start_address, "0xFFFF3000");
    EXPECT_EQ(checksum_selection_.make, "Nissan");
    EXPECT_EQ(checksum_selection_.flash_method, "proto_a");
    EXPECT_EQ(checksum_selection_.checksum_flag, "yes");
    EXPECT_EQ(checksum_selection_.mcu_type, "SH7058");
    EXPECT_EQ(checksum_selection_.rom_id, "TEST");
    EXPECT_TRUE(checksum_has_definition_);
    EXPECT_THAT(trace_, ElementsAre("protocol:Protocol A", "checksum"));
    EXPECT_EQ(prepared->protocol.name, "proto_a");

    // The description follows the two reselection logs and precedes the
    // kernel/MCU fill.
    ASSERT_THAT(descriptions_, SizeIs(1));
    EXPECT_EQ(descriptions_[0].logs_before, 2U);
    EXPECT_EQ(descriptions_[0].session_protocol.flash_method, "proto_a");
    EXPECT_EQ(descriptions_[0].session_protocol.mcu_type, "");
    EXPECT_EQ(descriptions_[0].session_protocol.kernel_path, "");
    EXPECT_THAT(logs_,
                ElementsAre(Pair(LogLevel::kDebug, "Update protocol info by selected ROM with FlashMethod: proto_a"),
                            Pair(LogLevel::kDebug, "Protocol info for selected ROM updated"),
                            Pair(LogLevel::kDebug, "Protocol: proto_a"), Pair(LogLevel::kDebug, "Make: Nissan"),
                            Pair(LogLevel::kDebug, "Checksum: yes"),
                            Pair(LogLevel::kDebug, "ecuCalDef->McuType: SH7058 SH7058"),
                            Pair(LogLevel::kDebug, "Size: 0x3 -> 0x100000")));
}

struct UncorrectedCase
{
    std::string_view name;
    ChecksumCorrectionResult result;
    bool logs_cancellation;
    bool logs_unknown_mcu;
};

void PrintTo(const UncorrectedCase& param, std::ostream *os)
{
    *os << param.name;
}

std::vector<UncorrectedCase> uncorrectedCases()
{
    return {UncorrectedCase{"NoCorrectedBytes", {}, false, false},
            UncorrectedCase{"DeclinedMissingModule", {.canceled_due_to_missing_module = true}, true, false},
            UncorrectedCase{"UnknownMcu", {.unknown_mcu_type = true}, false, true},
            // Unknown MCU returns before the cancellation log and before any
            // corrected bytes are applied.
            UncorrectedCase{"UnknownMcuTakesPrecedence",
                            {.corrected_rom_data = bytes::Bytes{4, 5, 6},
                             .canceled_due_to_missing_module = true,
                             .unknown_mcu_type = true},
                            false,
                            true}};
}

class UncorrectedWriteStillPrepares : public CoordinatorHarness<::testing::TestWithParam<UncorrectedCase>>
{
};

TEST_P(UncorrectedWriteStillPrepares, WithOriginalBytes)
{
    expectChecksum(GetParam().result);

    const std::optional<PreparedWrite> prepared = coordinator_.prepareWrite(&session_, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    EXPECT_THAT(prepared->image, ElementsAre(9, 2, 3));
    EXPECT_THAT(session_.Rom(), ElementsAre(9, 2, 3));
    EXPECT_THAT(
        logs_,
        Contains(Pair(LogLevel::kDebug, "Checksum calculation canceled!")).Times(GetParam().logs_cancellation ? 1 : 0));
    EXPECT_THAT(
        logs_, Contains(Pair(LogLevel::kError, "Unknown MCU type: SH7058")).Times(GetParam().logs_unknown_mcu ? 1 : 0));
}

INSTANTIATE_TEST_SUITE_P(Outcomes, UncorrectedWriteStillPrepares, ::testing::ValuesIn(uncorrectedCases()),
                         [](const ::testing::TestParamInfo<UncorrectedCase>& info)
                         { return std::string{info.param.name}; });

struct NoReselectCase
{
    std::string_view name;
    bool has_definition;
    std::string flash_method;
};

void PrintTo(const NoReselectCase& param, std::ostream *os)
{
    *os << param.name;
}

class DefinitionlessOrNonemptyMethodDoesNotReselect
    : public CoordinatorHarness<::testing::TestWithParam<NoReselectCase>>
{
};

TEST_P(DefinitionlessOrNonemptyMethodDoesNotReselect, ButRefreshesKernelAndMcu)
{
    ASSERT_NO_FATAL_FAILURE(openSession(
        GetParam().has_definition ? std::optional{headerOnlyDefinition()} : std::nullopt, GetParam().flash_method));
    expectChecksum({});

    const std::optional<PreparedWrite> prepared = coordinator_.prepareWrite(&session_, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(session_.Protocol().flash_method, GetParam().flash_method);
    // A reselection by "proto_a" would have moved to row 2.
    ASSERT_THAT(cfg_.session.SelectedRow(), IsOk());
    EXPECT_EQ(*cfg_.session.SelectedRow(), 0U);
    EXPECT_THAT(descriptions_, IsEmpty());
    EXPECT_EQ(session_.Protocol().mcu_type, "SH7058");
    EXPECT_EQ(session_.Protocol().kernel_path, "/kernels/a.bin");
    EXPECT_EQ(session_.Protocol().kernel_start_address, "0xFFFF3000");
    EXPECT_EQ(checksum_has_definition_, GetParam().has_definition);
    EXPECT_EQ(checksum_selection_.flash_method, "proto_a");
    EXPECT_EQ(prepared->protocol.name, "proto_a");
}

INSTANTIATE_TEST_SUITE_P(Methods, DefinitionlessOrNonemptyMethodDoesNotReselect,
                         ::testing::Values(NoReselectCase{"DefinitionlessEmpty", false, ""},
                                           NoReselectCase{"DefinedDifferentProtocol", true, "different_protocol"},
                                           NoReselectCase{"DefinedPlaceholder", true, " "}),
                         [](const ::testing::TestParamInfo<NoReselectCase>& info)
                         { return std::string{info.param.name}; });

struct KernelDirectoryCase
{
    std::string_view name;
    std::string_view directory;
    std::string_view expected_path;
};

void PrintTo(const KernelDirectoryCase& param, std::ostream *os)
{
    *os << param.name;
}

class KernelDirectoryJoining : public CoordinatorHarness<::testing::TestWithParam<KernelDirectoryCase>>
{
};

TEST_P(KernelDirectoryJoining, MatchesFlashKernelPath)
{
    expectChecksum({});

    const std::optional<PreparedWrite> prepared = coordinator_.prepareWrite(&session_, GetParam().directory);

    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(session_.Protocol().kernel_path, GetParam().expected_path);
    EXPECT_EQ(prepared->kernel_path, GetParam().expected_path);
}

INSTANTIATE_TEST_SUITE_P(Directories, KernelDirectoryJoining,
                         ::testing::Values(KernelDirectoryCase{"NoTrailingSlash", "/kernels", "/kernels/a.bin"},
                                           KernelDirectoryCase{"TrailingSlash", "/kernels/", "/kernels/a.bin"},
                                           KernelDirectoryCase{"Empty", "", "a.bin"}),
                         [](const ::testing::TestParamInfo<KernelDirectoryCase>& info)
                         { return std::string{info.param.name}; });

std::string_view modeName(SaveMode mode)
{
    return mode == SaveMode::kSave ? "Save" : "SaveAs";
}

std::string modeParamName(const ::testing::TestParamInfo<SaveMode>& info)
{
    return std::string{modeName(info.param)};
}

class MissingSaveSelection : public CoordinatorHarness<::testing::TestWithParam<SaveMode>>
{
};

TEST_P(MissingSaveSelection, OnlyNotifies)
{
    EXPECT_CALL(interaction_, showNotice(CalibrationNotice::kNoCalibrationToSave));

    EXPECT_EQ(coordinator_.save(nullptr, GetParam()), SaveOutcome::kNoSelection);

    EXPECT_THAT(logs_, IsEmpty());
    EXPECT_THAT(cfg_.file_repository.write_calls, IsEmpty());
    EXPECT_THAT(cfg_.events.notices, IsEmpty());
}

INSTANTIATE_TEST_SUITE_P(Modes, MissingSaveSelection, ::testing::Values(SaveMode::kSave, SaveMode::kSaveAs),
                         modeParamName);

TEST_F(CalibrationOperationCoordinator, SavePersistsCorrectedCopy)
{
    expectChecksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});

    EXPECT_EQ(coordinator_.save(&session_, SaveMode::kSave), SaveOutcome::kSaved);
    EXPECT_THAT(cfg_.file_repository.files.at("/old/read.bin"), ElementsAre(4, 5, 6));
    EXPECT_THAT(session_.Rom(), ElementsAre(9, 2, 3));
    EXPECT_EQ(session_.Source().origin, calibration::RomOrigin::kEcuRead);
    EXPECT_FALSE(session_.Dirty());
    EXPECT_THAT(logs_, Contains(Pair(LogLevel::kDebug, "ecuCalDef->FileName: read.bin")));
    EXPECT_THAT(logs_, Contains(Pair(LogLevel::kDebug, "ecuCalDef->FullFileName: /old/read.bin")));

    EXPECT_THAT(checksum_image_, ElementsAre(9, 2, 3));
    EXPECT_THAT(cfg_.file_repository.write_calls, SizeIs(1));
    // The Save As progress lines belong to the picker flow only.
    EXPECT_THAT(logs_, Not(Contains(Pair(_, StartsWith("Save as:")))));
    EXPECT_THAT(cfg_.events.notices, IsEmpty());
}

using UncorrectedSaveParam = std::tuple<UncorrectedCase, SaveMode>;

class UncorrectedSaveStillPersists : public CoordinatorHarness<::testing::TestWithParam<UncorrectedSaveParam>>
{
};

TEST_P(UncorrectedSaveStillPersists, WithOriginalBytes)
{
    const auto& [outcome, mode] = GetParam();
    expectChecksum(outcome.result);
    const std::string destination = expectDestination(mode);

    EXPECT_EQ(coordinator_.save(&session_, mode), SaveOutcome::kSaved);

    EXPECT_THAT(cfg_.file_repository.files.at(destination), ElementsAre(9, 2, 3));
    EXPECT_THAT(session_.Rom(), ElementsAre(9, 2, 3));
    EXPECT_FALSE(session_.Dirty());
    EXPECT_THAT(
        logs_,
        Contains(Pair(LogLevel::kDebug, "Checksum calculation canceled!")).Times(outcome.logs_cancellation ? 1 : 0));
    // Saving never refreshes the MCU, so the read session's empty one is named.
    EXPECT_THAT(logs_, Contains(Pair(LogLevel::kError, "Unknown MCU type: ")).Times(outcome.logs_unknown_mcu ? 1 : 0));
}

INSTANTIATE_TEST_SUITE_P(
    Outcomes, UncorrectedSaveStillPersists,
    ::testing::Combine(::testing::ValuesIn(uncorrectedCases()), ::testing::Values(SaveMode::kSave, SaveMode::kSaveAs)),
    [](const ::testing::TestParamInfo<UncorrectedSaveParam>& info)
    { return std::format("{}_{}", std::get<0>(info.param).name, modeName(std::get<1>(info.param))); });

class SaveDoesNotRefreshWriteMetadata : public CoordinatorHarness<::testing::TestWithParam<SaveMode>>
{
};

TEST_P(SaveDoesNotRefreshWriteMetadata, EvenForEmptyDefinedMethod)
{
    // A write would fill this empty method and reselect row 2 by it.
    ASSERT_NO_FATAL_FAILURE(openSession(headerOnlyDefinition(), {}));
    const calibration::RomProtocolInfo protocolBefore = session_.Protocol();
    expectChecksum({});
    expectDestination(GetParam());

    EXPECT_EQ(coordinator_.save(&session_, GetParam()), SaveOutcome::kSaved);

    EXPECT_EQ(session_.Protocol(), protocolBefore);
    ASSERT_THAT(cfg_.session.SelectedRow(), IsOk());
    EXPECT_EQ(*cfg_.session.SelectedRow(), 0U);
    EXPECT_THAT(descriptions_, IsEmpty());
    EXPECT_TRUE(checksum_has_definition_);
    EXPECT_EQ(checksum_selection_.mcu_type, "");
}

INSTANTIATE_TEST_SUITE_P(Modes, SaveDoesNotRefreshWriteMetadata, ::testing::Values(SaveMode::kSave, SaveMode::kSaveAs),
                         modeParamName);

TEST_F(CalibrationOperationCoordinator, SaveAsCorrectsBeforeChoosingPath)
{
    // A configured directory that differs from the provisioned one shows the
    // suggestion comes from the effective paths.
    cfg_.PutSettings(config::testing::Setting("calibration_files_directory", "/cal"));
    ASSERT_NO_FATAL_FAILURE(start(config::testing::kStandardCatalog));
    ASSERT_NE(cfg_.paths.calibration_files_directory, "/cal/");
    expectChecksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});
    expectChoose("/cal/saved.bin");

    EXPECT_EQ(coordinator_.save(&session_, SaveMode::kSaveAs), SaveOutcome::kSaved);

    EXPECT_THAT(trace_, ElementsAre("checksum", "choose"));
    EXPECT_EQ(suggested_path_, cfg_.session.EffectivePaths().calibration_files_directory + "read.bin");
    EXPECT_EQ(suggested_path_, "/cal/read.bin");
    EXPECT_THAT(cfg_.file_repository.files.at("/cal/saved.bin"), ElementsAre(4, 5, 6));
    EXPECT_THAT(session_.Rom(), ElementsAre(9, 2, 3));
    EXPECT_EQ(session_.Source().origin, calibration::RomOrigin::kEcuRead);
    // Each progress line precedes its step: the first the checksum logs, the
    // second the picker.
    EXPECT_EQ(logs_before_choose_, 5U);
    EXPECT_THAT(logs_, ElementsAre(Pair(LogLevel::kDebug, "Save as: Check selected ROM number"),
                                   Pair(LogLevel::kDebug, "Protocol: proto_a"), Pair(LogLevel::kDebug, "Make: Subaru"),
                                   Pair(LogLevel::kDebug, "Checksum: yes"),
                                   Pair(LogLevel::kDebug, "Save as: Check if OEM ECU file"),
                                   Pair(LogLevel::kDebug, "ecuCalDef->FileName: saved.bin"),
                                   Pair(LogLevel::kDebug, "ecuCalDef->FullFileName: /cal/saved.bin")));
}

// Q3: a file is saved in the layout it was opened in; the fill bytes Rom()
// shows for the RAM range never reach the checksum image or the saved file.
TEST_F(CalibrationOperationCoordinator, SaveWritesTheRomFileInTheLayoutItWasOpenedIn)
{
    calibration::CalibrationSession packed = packedFile();
    ASSERT_THAT(packed.Rom(), ElementsAre(1, 2, 0xFF, 0xFF, 3, 4));
    expectChecksum({});

    EXPECT_EQ(coordinator_.save(&packed, SaveMode::kSave), SaveOutcome::kSaved);

    EXPECT_THAT(checksum_image_, ElementsAre(1, 2, 3, 4));
    EXPECT_THAT(cfg_.file_repository.files.at("/cal/packed.bin"), ElementsAre(1, 2, 3, 4));
}

TEST_F(CalibrationOperationCoordinator, WriteImagesAreTheRomFileAsOpened)
{
    calibration::CalibrationSession packed = packedFile();
    expectChecksum({});

    const std::optional<PreparedWrite> prepared = coordinator_.prepareWrite(&packed, "/kernels");

    ASSERT_TRUE(prepared.has_value());
    EXPECT_THAT(prepared->image, ElementsAre(1, 2, 3, 4));
}

struct CancelledPickCase
{
    std::string_view name;
    std::optional<std::string> chosen;
};

void PrintTo(const CancelledPickCase& param, std::ostream *os)
{
    *os << param.name;
}

class SaveAsCancelledAfterCorrection : public CoordinatorHarness<::testing::TestWithParam<CancelledPickCase>>
{
};

TEST_P(SaveAsCancelledAfterCorrection, WritesNothing)
{
    const calibration::RomSource sourceBefore = session_.Source();
    expectChecksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});
    expectChoose(GetParam().chosen);
    EXPECT_CALL(interaction_, showNotice(CalibrationNotice::kNoSaveFilename))
        .WillOnce([this] { trace_.emplace_back("notice"); });

    EXPECT_EQ(coordinator_.save(&session_, SaveMode::kSaveAs), SaveOutcome::kCancelled);

    EXPECT_THAT(trace_, ElementsAre("checksum", "choose", "notice"));
    EXPECT_EQ(session_.Source(), sourceBefore);
    EXPECT_TRUE(session_.Dirty());
    EXPECT_THAT(session_.Rom(), ElementsAre(9, 2, 3));
    EXPECT_THAT(cfg_.file_repository.write_calls, IsEmpty());
    EXPECT_THAT(cfg_.events.notices, IsEmpty());
    EXPECT_THAT(logs_, Not(Contains(Pair(_, StartsWith("ecuCalDef->")))));
    EXPECT_THAT(logs_, Not(Contains(Pair(LogLevel::kError, _))));
}

INSTANTIATE_TEST_SUITE_P(Pickers, SaveAsCancelledAfterCorrection,
                         ::testing::Values(CancelledPickCase{"Dismissed", std::nullopt},
                                           CancelledPickCase{"EmptyPath", std::string{}}),
                         [](const ::testing::TestParamInfo<CancelledPickCase>& info)
                         { return std::string{info.param.name}; });

struct FilenameCase
{
    std::string_view name;
    std::string selected;
    std::string expected_path;
    std::string expected_basename;
};

void PrintTo(const FilenameCase& param, std::ostream *os)
{
    *os << param.name;
}

class SaveAsFilenameCompatibility : public CoordinatorHarness<::testing::TestWithParam<FilenameCase>>
{
};

TEST_P(SaveAsFilenameCompatibility, PersistsLegacySuffix)
{
    const std::string& expectedPath = GetParam().expected_path;
    const std::string& expectedBasename = GetParam().expected_basename;
    expectChecksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});
    expectChoose(GetParam().selected);

    EXPECT_EQ(coordinator_.save(&session_, SaveMode::kSaveAs), SaveOutcome::kSaved);
    EXPECT_EQ(session_.Source().path, expectedPath);
    EXPECT_EQ(session_.Source().display_name, expectedBasename);
    EXPECT_FALSE(session_.Dirty());
    EXPECT_THAT(cfg_.file_repository.files.at(expectedPath), ElementsAre(4, 5, 6));
    EXPECT_THAT(trace_, ElementsAre("checksum", "choose"));

    EXPECT_THAT(cfg_.file_repository.write_calls, SizeIs(1));
    EXPECT_EQ(session_.Source().origin, calibration::RomOrigin::kEcuRead);
    EXPECT_THAT(logs_, Contains(Pair(LogLevel::kDebug, "ecuCalDef->FullFileName: " + expectedPath)));
}

// Selected path -> persisted path. Only one trailing dot is removed, and the
// ".bin" comparison is case-sensitive. The UTF-8 bytes of "é" are spelled out
// so the literal does not depend on the compiler's source charset.
INSTANTIATE_TEST_SUITE_P(
    Destinations, SaveAsFilenameCompatibility,
    ::testing::Values(FilenameCase{"TrailingDot", "/cal/renamed.", "/cal/renamed.bin", "renamed.bin"},
                      FilenameCase{"NoSuffix", "/cal/renamed", "/cal/renamed.bin", "renamed.bin"},
                      FilenameCase{"LowercaseBin", "/cal/renamed.bin", "/cal/renamed.bin", "renamed.bin"},
                      FilenameCase{"UppercaseBin", "/cal/renamed.BIN", "/cal/renamed.BIN.bin", "renamed.BIN.bin"},
                      FilenameCase{"Utf8", "/cal/tune-\xc3\xa9.bin", "/cal/tune-\xc3\xa9.bin", "tune-\xc3\xa9.bin"},
                      FilenameCase{"TwoTrailingDots", "/cal/renamed..", "/cal/renamed..bin", "renamed..bin"}),
    [](const ::testing::TestParamInfo<FilenameCase>& info) { return std::string{info.param.name}; });

using FailedSaveParam = std::tuple<SaveMode, bool>; // mode, initially dirty

class FailedSavePreservesState : public CoordinatorHarness<::testing::TestWithParam<FailedSaveParam>>
{
};

TEST_P(FailedSavePreservesState, AndReportsOnce)
{
    const auto [mode, initiallyDirty] = GetParam();
    if (!initiallyDirty)
    {
        session_ = ecuRead(std::nullopt, {});
    }
    // Save As names the normalized destination, not the picker's text.
    const std::string target = mode == SaveMode::kSave ? "/old/read.bin" : "/cal/fail.bin";
    if (mode == SaveMode::kSaveAs)
    {
        expectChoose("/cal/fail");
    }
    cfg_.file_repository.write_errors[target] = Error{ErrorKind::kInternal, "disk full"};
    const calibration::RomSource sourceBefore = session_.Source();
    const bytes::Bytes romBefore(session_.Rom().begin(), session_.Rom().end());
    expectChecksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});

    EXPECT_EQ(coordinator_.save(&session_, mode), SaveOutcome::kFailed);

    EXPECT_EQ(session_.Source(), sourceBefore);
    EXPECT_THAT(session_.Rom(), ElementsAreArray(romBefore));
    EXPECT_EQ(session_.Dirty(), initiallyDirty);
    EXPECT_THAT(cfg_.file_repository.write_calls, IsEmpty());
    EXPECT_THAT(logs_, Not(Contains(Pair(_, StartsWith("ecuCalDef->")))));
    EXPECT_THAT(cfg_.events.logs,
                ElementsAre(Pair(LogLevel::kError, "Unable to open file " + target + " for writing")));
    EXPECT_THAT(cfg_.events.notices,
                ElementsAre("Ecu calibration file: Unable to open file " + target + " for writing"));
    EXPECT_THAT(logs_, Contains(Pair(LogLevel::kError, "Calibration file not saved: " + target)).Times(1));
}

INSTANTIATE_TEST_SUITE_P(States, FailedSavePreservesState,
                         ::testing::Combine(::testing::Values(SaveMode::kSave, SaveMode::kSaveAs), ::testing::Bool()),
                         [](const ::testing::TestParamInfo<FailedSaveParam>& info)
                         {
                             return std::format("{}_{}", modeName(std::get<0>(info.param)),
                                                std::get<1>(info.param) ? "Dirty" : "Clean");
                         });

} // namespace
