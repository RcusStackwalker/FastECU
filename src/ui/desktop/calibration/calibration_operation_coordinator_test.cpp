#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"

#include <cstddef>
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

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/rom_save.h"
#include "src/backend/checksum/checksum_selection.h"
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

calibration::ResolvedDefinition header_only_definition()
{
    return calibration::ResolvedDefinition{.id = "HEADER"};
}

// An ECU read of {1, 2, 3}; open_session() dirties it to {9, 2, 3}.
calibration::CalibrationSession ecu_read(std::optional<calibration::ResolvedDefinition> definition,
                                         std::string flash_method)
{
    return calibration::CalibrationSession{
        calibration::SessionId{1},
        calibration::SessionContents{
            .source = {.display_name = "read.bin", .path = "/old/read.bin", .origin = calibration::RomOrigin::EcuRead},
            .rom = {1, 2, 3},
            .definition = std::move(definition),
            .protocol = {.flash_method = std::move(flash_method), .rom_id = "TEST"},
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
        ASSERT_NO_FATAL_FAILURE(start(config::testing::kStandardProtocols));
        ASSERT_NO_FATAL_FAILURE(open_session(std::nullopt, {}));
    }

    // Initializes the configuration over `protocols`, selects row 0, and drops
    // what initialization recorded so a case sees only its own operation.
    void start(std::string_view protocols)
    {
        cfg.put_protocols(protocols);
        ASSERT_THAT(cfg.initialize(), IsOk());
        ASSERT_THAT(cfg.session.select_row(0), IsOk());
        cfg.file_repository.read_handles.clear();
        cfg.file_repository.write_calls.clear();
        cfg.events.logs.clear();
        cfg.events.notices.clear();
    }

    void open_session(std::optional<calibration::ResolvedDefinition> definition, std::string flash_method)
    {
        session = ecu_read(std::move(definition), std::move(flash_method));
        ASSERT_THAT(session.write_bytes(0, bytes::Bytes{9}), IsOk());
    }

    // Scripts one checksum interaction returning `result`, recording its
    // arguments and its place in the callback trace.
    void expect_checksum(ChecksumCorrectionResult result)
    {
        EXPECT_CALL(interaction, correct_checksums(_, _, _))
            .WillOnce(
                [this, result](bytes::ByteView image, bool has_definition, const checksum::ChecksumSelection& selection)
                {
                    checksum_image.assign(image.begin(), image.end());
                    checksum_has_definition = has_definition;
                    checksum_selection = selection;
                    trace.emplace_back("checksum");
                    return result;
                });
    }

    // Scripts one Save As picker returning `chosen`, recording the suggested
    // path, how many logs preceded it, and its place in the callback trace.
    // Nothing may reach the repository before a destination is chosen.
    void expect_choose(std::optional<std::string> chosen)
    {
        EXPECT_CALL(interaction, choose_save_path(_))
            .WillOnce(
                [this, chosen](std::string_view suggested)
                {
                    EXPECT_THAT(cfg.file_repository.write_calls, IsEmpty());
                    suggested_path = suggested;
                    logs_before_choose = logs.size();
                    trace.emplace_back("choose");
                    return chosen;
                });
    }

    // Where a save in `mode` persists: the session's own path for Save, or
    // `chosen` from a scripted picker for Save As.
    std::string expect_destination(SaveMode mode, const std::string& chosen = "/cal/saved.bin")
    {
        if (mode == SaveMode::Save)
        {
            return session.source().path;
        }
        expect_choose(chosen);
        return chosen;
    }

    std::vector<LogLine> logs;
    std::vector<DescriptionEvent> descriptions;
    std::vector<std::string> trace;
    bytes::Bytes checksum_image;
    bool checksum_has_definition = false;
    checksum::ChecksumSelection checksum_selection;
    std::string suggested_path;
    std::size_t logs_before_choose = 0;

    config::testing::ConfigSessionFixture cfg;
    calibration::RomSaveUseCase saver{cfg.file_repository, cfg.events};
    StrictMock<MockCalibrationInteraction> interaction;
    calibration::CalibrationSession session = ecu_read(std::nullopt, {});
    ui::CalibrationOperationCoordinator coordinator{
        cfg.session, saver, interaction,
        ui::CalibrationPresentationCallbacks{
            .log = [this](LogLevel level, std::string_view text) { logs.emplace_back(level, text); },
            .protocol_description_changed =
                [this](std::string_view description)
            {
                descriptions.push_back({std::string{description}, logs.size(), session.protocol()});
                trace.push_back(std::format("protocol:{}", description));
            },
        }};
};

class CalibrationOperationCoordinator : public CoordinatorHarness<::testing::Test>
{
};

TEST_F(CalibrationOperationCoordinator, MissingWriteSelection)
{
    EXPECT_CALL(interaction, show_notice(CalibrationNotice::NoCalibrationToWrite));

    EXPECT_EQ(coordinator.prepare_write(nullptr, "/kernels/"), std::nullopt);

    EXPECT_THAT(logs, IsEmpty());
    EXPECT_THAT(descriptions, IsEmpty());
}

TEST_F(CalibrationOperationCoordinator, CancelledWriteWarning)
{
    ASSERT_THAT(cfg.session.select_row(1), IsOk()); // proto_b: checksum n/a
    const calibration::RomProtocolInfo protocol_before = session.protocol();
    EXPECT_CALL(interaction, confirm_write_without_checksum()).WillOnce(Return(false));

    EXPECT_EQ(coordinator.prepare_write(&session, "/kernels/"), std::nullopt);

    // The refresh would have filled the MCU and kernel fields.
    EXPECT_EQ(session.protocol(), protocol_before);
    EXPECT_THAT(logs, Contains(Pair(LogLevel::Debug, "Write canceled!")));
    EXPECT_THAT(descriptions, IsEmpty());
}

TEST_F(CalibrationOperationCoordinator, AcceptedWriteWarningSkipsCorrection)
{
    ASSERT_THAT(cfg.session.select_row(1), IsOk()); // proto_b: checksum n/a
    EXPECT_CALL(interaction, confirm_write_without_checksum()).WillOnce(Return(true));

    const std::optional<PreparedWrite> prepared = coordinator.prepare_write(&session, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    EXPECT_THAT(prepared->image, ElementsAre(9, 2, 3));
    EXPECT_EQ(prepared->protocol, "proto_b");
    EXPECT_EQ(prepared->mcu, "M32R");
    EXPECT_EQ(prepared->kernel_path, "/kernels/b.bin");
    EXPECT_EQ(prepared->display_filename, "read.bin");
    EXPECT_EQ(session.protocol().mcu_type, "M32R");
    EXPECT_THAT(logs, Not(Contains(Pair(LogLevel::Debug, "Write canceled!"))));
}

TEST_F(CalibrationOperationCoordinator, CorrectedWriteUsesOnlyOperationBytes)
{
    expect_checksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});

    const std::optional<PreparedWrite> prepared = coordinator.prepare_write(&session, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    EXPECT_THAT(checksum_image, ElementsAre(9, 2, 3));
    EXPECT_FALSE(checksum_has_definition);
    EXPECT_THAT(prepared->image, ElementsAre(4, 5, 6));
    EXPECT_THAT(session.rom(), ElementsAre(9, 2, 3));
    EXPECT_TRUE(session.dirty());
    EXPECT_EQ(prepared->protocol, "proto_a");
    EXPECT_EQ(prepared->mcu, "SH7058");
    EXPECT_EQ(prepared->kernel_path, "/kernels/a.bin");
    EXPECT_EQ(prepared->display_filename, "read.bin");
    EXPECT_THAT(cfg.file_repository.write_calls, IsEmpty());
    EXPECT_THAT(cfg.events.notices, IsEmpty());
}

TEST_F(CalibrationOperationCoordinator, ChecksumLogsKeepLegacyTextAndOrder)
{
    // 0x1a bytes, so the size line shows the lowercase hexadecimal legacy
    // QString::number(n, 16) produced.
    session = calibration::CalibrationSession{calibration::SessionId{2},
                                              calibration::SessionContents{.rom = bytes::Bytes(0x1a, 0)}};
    expect_checksum({.canceled_due_to_missing_module = true});

    ASSERT_TRUE(coordinator.prepare_write(&session, "/kernels/").has_value());

    EXPECT_THAT(logs, ElementsAre(Pair(LogLevel::Debug, "Protocol: proto_a"), Pair(LogLevel::Debug, "Make: Subaru"),
                                  Pair(LogLevel::Debug, "Checksum: yes"),
                                  Pair(LogLevel::Debug, "ecuCalDef->McuType: SH7058 SH7058"),
                                  Pair(LogLevel::Debug, "Size: 0x1a -> 0x100000"),
                                  Pair(LogLevel::Debug, "Checksum calculation canceled!")));
}

TEST_F(CalibrationOperationCoordinator, EmptyDefinedMethodReselectsBeforeChecksum)
{
    // Row 2 is the last proto_a row; making it a Nissan shows that the
    // checksum request reads the reselected vehicle, not row 0.
    constexpr std::string_view subaru_forester = "<make>Subaru</make><model>Forester</model>";
    std::string protocols{config::testing::kStandardProtocols};
    const std::size_t forester = protocols.find(subaru_forester);
    ASSERT_NE(forester, std::string::npos);
    protocols.replace(forester, subaru_forester.size(), "<make>Nissan</make><model>Forester</model>");
    ASSERT_NO_FATAL_FAILURE(start(protocols));
    ASSERT_NO_FATAL_FAILURE(open_session(header_only_definition(), {}));
    expect_checksum({});

    const std::optional<PreparedWrite> prepared = coordinator.prepare_write(&session, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    ASSERT_THAT(cfg.session.selected_row(), IsOk());
    EXPECT_EQ(*cfg.session.selected_row(), 2U);
    EXPECT_EQ(session.protocol().flash_method, "proto_a");
    EXPECT_EQ(session.protocol().mcu_type, "SH7058");
    EXPECT_EQ(session.protocol().kernel_path, "/kernels/a.bin");
    EXPECT_EQ(session.protocol().kernel_start_address, "0xFFFF3000");
    EXPECT_EQ(checksum_selection.make, "Nissan");
    EXPECT_EQ(checksum_selection.flash_method, "proto_a");
    EXPECT_EQ(checksum_selection.checksum_flag, "yes");
    EXPECT_EQ(checksum_selection.mcu_type, "SH7058");
    EXPECT_EQ(checksum_selection.rom_id, "TEST");
    EXPECT_TRUE(checksum_has_definition);
    EXPECT_THAT(trace, ElementsAre("protocol:Protocol A", "checksum"));
    EXPECT_EQ(prepared->protocol, "proto_a");

    // The description follows the two reselection logs and precedes the
    // kernel/MCU fill.
    ASSERT_THAT(descriptions, SizeIs(1));
    EXPECT_EQ(descriptions[0].logs_before, 2U);
    EXPECT_EQ(descriptions[0].session_protocol.flash_method, "proto_a");
    EXPECT_EQ(descriptions[0].session_protocol.mcu_type, "");
    EXPECT_EQ(descriptions[0].session_protocol.kernel_path, "");
    EXPECT_THAT(logs,
                ElementsAre(Pair(LogLevel::Debug, "Update protocol info by selected ROM with FlashMethod: proto_a"),
                            Pair(LogLevel::Debug, "Protocol info for selected ROM updated"),
                            Pair(LogLevel::Debug, "Protocol: proto_a"), Pair(LogLevel::Debug, "Make: Nissan"),
                            Pair(LogLevel::Debug, "Checksum: yes"),
                            Pair(LogLevel::Debug, "ecuCalDef->McuType: SH7058 SH7058"),
                            Pair(LogLevel::Debug, "Size: 0x3 -> 0x100000")));
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

std::vector<UncorrectedCase> uncorrected_cases()
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
    expect_checksum(GetParam().result);

    const std::optional<PreparedWrite> prepared = coordinator.prepare_write(&session, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    EXPECT_THAT(prepared->image, ElementsAre(9, 2, 3));
    EXPECT_THAT(session.rom(), ElementsAre(9, 2, 3));
    EXPECT_THAT(
        logs,
        Contains(Pair(LogLevel::Debug, "Checksum calculation canceled!")).Times(GetParam().logs_cancellation ? 1 : 0));
    EXPECT_THAT(logs,
                Contains(Pair(LogLevel::Error, "Unknown MCU type: SH7058")).Times(GetParam().logs_unknown_mcu ? 1 : 0));
}

INSTANTIATE_TEST_SUITE_P(Outcomes, UncorrectedWriteStillPrepares, ::testing::ValuesIn(uncorrected_cases()),
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
    ASSERT_NO_FATAL_FAILURE(open_session(
        GetParam().has_definition ? std::optional{header_only_definition()} : std::nullopt, GetParam().flash_method));
    expect_checksum({});

    const std::optional<PreparedWrite> prepared = coordinator.prepare_write(&session, "/kernels/");

    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(session.protocol().flash_method, GetParam().flash_method);
    // A reselection by "proto_a" would have moved to row 2.
    ASSERT_THAT(cfg.session.selected_row(), IsOk());
    EXPECT_EQ(*cfg.session.selected_row(), 0U);
    EXPECT_THAT(descriptions, IsEmpty());
    EXPECT_EQ(session.protocol().mcu_type, "SH7058");
    EXPECT_EQ(session.protocol().kernel_path, "/kernels/a.bin");
    EXPECT_EQ(session.protocol().kernel_start_address, "0xFFFF3000");
    EXPECT_EQ(checksum_has_definition, GetParam().has_definition);
    EXPECT_EQ(checksum_selection.flash_method, "proto_a");
    EXPECT_EQ(prepared->protocol, "proto_a");
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
    expect_checksum({});

    const std::optional<PreparedWrite> prepared = coordinator.prepare_write(&session, GetParam().directory);

    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(session.protocol().kernel_path, GetParam().expected_path);
    EXPECT_EQ(prepared->kernel_path, GetParam().expected_path);
}

INSTANTIATE_TEST_SUITE_P(Directories, KernelDirectoryJoining,
                         ::testing::Values(KernelDirectoryCase{"NoTrailingSlash", "/kernels", "/kernels/a.bin"},
                                           KernelDirectoryCase{"TrailingSlash", "/kernels/", "/kernels/a.bin"},
                                           KernelDirectoryCase{"Empty", "", "a.bin"}),
                         [](const ::testing::TestParamInfo<KernelDirectoryCase>& info)
                         { return std::string{info.param.name}; });

std::string_view mode_name(SaveMode mode)
{
    return mode == SaveMode::Save ? "Save" : "SaveAs";
}

std::string mode_param_name(const ::testing::TestParamInfo<SaveMode>& info)
{
    return std::string{mode_name(info.param)};
}

class MissingSaveSelection : public CoordinatorHarness<::testing::TestWithParam<SaveMode>>
{
};

TEST_P(MissingSaveSelection, OnlyNotifies)
{
    EXPECT_CALL(interaction, show_notice(CalibrationNotice::NoCalibrationToSave));

    EXPECT_EQ(coordinator.save(nullptr, GetParam()), SaveOutcome::NoSelection);

    EXPECT_THAT(logs, IsEmpty());
    EXPECT_THAT(cfg.file_repository.write_calls, IsEmpty());
    EXPECT_THAT(cfg.events.notices, IsEmpty());
}

INSTANTIATE_TEST_SUITE_P(Modes, MissingSaveSelection, ::testing::Values(SaveMode::Save, SaveMode::SaveAs),
                         mode_param_name);

TEST_F(CalibrationOperationCoordinator, SavePersistsCorrectedCopy)
{
    expect_checksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});

    EXPECT_EQ(coordinator.save(&session, SaveMode::Save), SaveOutcome::Saved);
    EXPECT_THAT(cfg.file_repository.files.at("/old/read.bin"), ElementsAre(4, 5, 6));
    EXPECT_THAT(session.rom(), ElementsAre(9, 2, 3));
    EXPECT_EQ(session.source().origin, calibration::RomOrigin::EcuRead);
    EXPECT_FALSE(session.dirty());
    EXPECT_THAT(logs, Contains(Pair(LogLevel::Debug, "ecuCalDef->FileName: read.bin")));
    EXPECT_THAT(logs, Contains(Pair(LogLevel::Debug, "ecuCalDef->FullFileName: /old/read.bin")));

    EXPECT_THAT(checksum_image, ElementsAre(9, 2, 3));
    EXPECT_THAT(cfg.file_repository.write_calls, SizeIs(1));
    // The Save As progress lines belong to the picker flow only.
    EXPECT_THAT(logs, Not(Contains(Pair(_, StartsWith("Save as:")))));
    EXPECT_THAT(cfg.events.notices, IsEmpty());
}

using UncorrectedSaveParam = std::tuple<UncorrectedCase, SaveMode>;

class UncorrectedSaveStillPersists : public CoordinatorHarness<::testing::TestWithParam<UncorrectedSaveParam>>
{
};

TEST_P(UncorrectedSaveStillPersists, WithOriginalBytes)
{
    const auto& [outcome, mode] = GetParam();
    expect_checksum(outcome.result);
    const std::string destination = expect_destination(mode);

    EXPECT_EQ(coordinator.save(&session, mode), SaveOutcome::Saved);

    EXPECT_THAT(cfg.file_repository.files.at(destination), ElementsAre(9, 2, 3));
    EXPECT_THAT(session.rom(), ElementsAre(9, 2, 3));
    EXPECT_FALSE(session.dirty());
    EXPECT_THAT(
        logs,
        Contains(Pair(LogLevel::Debug, "Checksum calculation canceled!")).Times(outcome.logs_cancellation ? 1 : 0));
    // Saving never refreshes the MCU, so the read session's empty one is named.
    EXPECT_THAT(logs, Contains(Pair(LogLevel::Error, "Unknown MCU type: ")).Times(outcome.logs_unknown_mcu ? 1 : 0));
}

INSTANTIATE_TEST_SUITE_P(
    Outcomes, UncorrectedSaveStillPersists,
    ::testing::Combine(::testing::ValuesIn(uncorrected_cases()), ::testing::Values(SaveMode::Save, SaveMode::SaveAs)),
    [](const ::testing::TestParamInfo<UncorrectedSaveParam>& info)
    { return std::format("{}_{}", std::get<0>(info.param).name, mode_name(std::get<1>(info.param))); });

class SaveDoesNotRefreshWriteMetadata : public CoordinatorHarness<::testing::TestWithParam<SaveMode>>
{
};

TEST_P(SaveDoesNotRefreshWriteMetadata, EvenForEmptyDefinedMethod)
{
    // A write would fill this empty method and reselect row 2 by it.
    ASSERT_NO_FATAL_FAILURE(open_session(header_only_definition(), {}));
    const calibration::RomProtocolInfo protocol_before = session.protocol();
    expect_checksum({});
    expect_destination(GetParam());

    EXPECT_EQ(coordinator.save(&session, GetParam()), SaveOutcome::Saved);

    EXPECT_EQ(session.protocol(), protocol_before);
    ASSERT_THAT(cfg.session.selected_row(), IsOk());
    EXPECT_EQ(*cfg.session.selected_row(), 0U);
    EXPECT_THAT(descriptions, IsEmpty());
    EXPECT_TRUE(checksum_has_definition);
    EXPECT_EQ(checksum_selection.mcu_type, "");
}

INSTANTIATE_TEST_SUITE_P(Modes, SaveDoesNotRefreshWriteMetadata, ::testing::Values(SaveMode::Save, SaveMode::SaveAs),
                         mode_param_name);

TEST_F(CalibrationOperationCoordinator, SaveAsCorrectsBeforeChoosingPath)
{
    // A configured directory that differs from the provisioned one shows the
    // suggestion comes from the effective paths.
    cfg.put_settings(config::testing::setting("calibration_files_directory", "/cal"));
    ASSERT_NO_FATAL_FAILURE(start(config::testing::kStandardProtocols));
    ASSERT_NE(cfg.paths.calibration_files_directory, "/cal/");
    expect_checksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});
    expect_choose("/cal/saved.bin");

    EXPECT_EQ(coordinator.save(&session, SaveMode::SaveAs), SaveOutcome::Saved);

    EXPECT_THAT(trace, ElementsAre("checksum", "choose"));
    EXPECT_EQ(suggested_path, cfg.session.effective_paths().calibration_files_directory + "read.bin");
    EXPECT_EQ(suggested_path, "/cal/read.bin");
    EXPECT_THAT(cfg.file_repository.files.at("/cal/saved.bin"), ElementsAre(4, 5, 6));
    EXPECT_THAT(session.rom(), ElementsAre(9, 2, 3));
    EXPECT_EQ(session.source().origin, calibration::RomOrigin::EcuRead);
    // Each progress line precedes its step: the first the checksum logs, the
    // second the picker.
    EXPECT_EQ(logs_before_choose, 5U);
    EXPECT_THAT(logs, ElementsAre(Pair(LogLevel::Debug, "Save as: Check selected ROM number"),
                                  Pair(LogLevel::Debug, "Protocol: proto_a"), Pair(LogLevel::Debug, "Make: Subaru"),
                                  Pair(LogLevel::Debug, "Checksum: yes"),
                                  Pair(LogLevel::Debug, "Save as: Check if OEM ECU file"),
                                  Pair(LogLevel::Debug, "ecuCalDef->FileName: saved.bin"),
                                  Pair(LogLevel::Debug, "ecuCalDef->FullFileName: /cal/saved.bin")));
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
    const calibration::RomSource source_before = session.source();
    expect_checksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});
    expect_choose(GetParam().chosen);
    EXPECT_CALL(interaction, show_notice(CalibrationNotice::NoSaveFilename))
        .WillOnce([this] { trace.emplace_back("notice"); });

    EXPECT_EQ(coordinator.save(&session, SaveMode::SaveAs), SaveOutcome::Cancelled);

    EXPECT_THAT(trace, ElementsAre("checksum", "choose", "notice"));
    EXPECT_EQ(session.source(), source_before);
    EXPECT_TRUE(session.dirty());
    EXPECT_THAT(session.rom(), ElementsAre(9, 2, 3));
    EXPECT_THAT(cfg.file_repository.write_calls, IsEmpty());
    EXPECT_THAT(cfg.events.notices, IsEmpty());
    EXPECT_THAT(logs, Not(Contains(Pair(_, StartsWith("ecuCalDef->")))));
    EXPECT_THAT(logs, Not(Contains(Pair(LogLevel::Error, _))));
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
    const std::string& expected_path = GetParam().expected_path;
    const std::string& expected_basename = GetParam().expected_basename;
    expect_checksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});
    expect_choose(GetParam().selected);

    EXPECT_EQ(coordinator.save(&session, SaveMode::SaveAs), SaveOutcome::Saved);
    EXPECT_EQ(session.source().path, expected_path);
    EXPECT_EQ(session.source().display_name, expected_basename);
    EXPECT_FALSE(session.dirty());
    EXPECT_THAT(cfg.file_repository.files.at(expected_path), ElementsAre(4, 5, 6));
    EXPECT_THAT(trace, ElementsAre("checksum", "choose"));

    EXPECT_THAT(cfg.file_repository.write_calls, SizeIs(1));
    EXPECT_EQ(session.source().origin, calibration::RomOrigin::EcuRead);
    EXPECT_THAT(logs, Contains(Pair(LogLevel::Debug, "ecuCalDef->FullFileName: " + expected_path)));
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
    const auto [mode, initially_dirty] = GetParam();
    if (!initially_dirty)
    {
        session = ecu_read(std::nullopt, {});
    }
    // Save As names the normalized destination, not the picker's text.
    const std::string target = mode == SaveMode::Save ? "/old/read.bin" : "/cal/fail.bin";
    if (mode == SaveMode::SaveAs)
    {
        expect_choose("/cal/fail");
    }
    cfg.file_repository.write_errors[target] = Error{ErrorKind::Internal, "disk full"};
    const calibration::RomSource source_before = session.source();
    const bytes::Bytes rom_before(session.rom().begin(), session.rom().end());
    expect_checksum({.corrected_rom_data = bytes::Bytes{4, 5, 6}});

    EXPECT_EQ(coordinator.save(&session, mode), SaveOutcome::Failed);

    EXPECT_EQ(session.source(), source_before);
    EXPECT_THAT(session.rom(), ElementsAreArray(rom_before));
    EXPECT_EQ(session.dirty(), initially_dirty);
    EXPECT_THAT(cfg.file_repository.write_calls, IsEmpty());
    EXPECT_THAT(logs, Not(Contains(Pair(_, StartsWith("ecuCalDef->")))));
    EXPECT_THAT(cfg.events.logs, ElementsAre(Pair(LogLevel::Error, "Unable to open file " + target + " for writing")));
    EXPECT_THAT(cfg.events.notices,
                ElementsAre("Ecu calibration file: Unable to open file " + target + " for writing"));
    EXPECT_THAT(logs, Contains(Pair(LogLevel::Error, "Calibration file not saved: " + target)).Times(1));
}

INSTANTIATE_TEST_SUITE_P(States, FailedSavePreservesState,
                         ::testing::Combine(::testing::Values(SaveMode::Save, SaveMode::SaveAs), ::testing::Bool()),
                         [](const ::testing::TestParamInfo<FailedSaveParam>& info)
                         {
                             return std::format("{}_{}", mode_name(std::get<0>(info.param)),
                                                std::get<1>(info.param) ? "Dirty" : "Clean");
                         });

} // namespace
