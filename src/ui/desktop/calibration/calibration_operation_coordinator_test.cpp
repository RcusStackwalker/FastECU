#include "src/ui/desktop/calibration/calibration_operation_coordinator.h"

#include <cstddef>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/rom_save.h"
#include "src/backend/checksum/checksum_selection.h"
#include "src/backend/config/testing/config_session_fixture.h"
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
using fastecu::LogLevel;
using fastecu::testing::IsOk;
using ::testing::_;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Pair;
using ::testing::Return;
using ::testing::SizeIs;
using ::testing::StrictMock;
using ui::CalibrationNotice;
using ui::ChecksumCorrectionResult;
using ui::MockCalibrationInteraction;
using ui::PreparedWrite;

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

    std::vector<LogLine> logs;
    std::vector<DescriptionEvent> descriptions;
    std::vector<std::string> trace;
    bytes::Bytes checksum_image;
    bool checksum_has_definition = false;
    checksum::ChecksumSelection checksum_selection;

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

INSTANTIATE_TEST_SUITE_P(
    Outcomes, UncorrectedWriteStillPrepares,
    ::testing::Values(UncorrectedCase{"NoCorrectedBytes", {}, false, false},
                      UncorrectedCase{"DeclinedMissingModule", {.canceled_due_to_missing_module = true}, true, false},
                      UncorrectedCase{"UnknownMcu", {.unknown_mcu_type = true}, false, true},
                      // Unknown MCU returns before the cancellation log and
                      // before any corrected bytes are applied.
                      UncorrectedCase{"UnknownMcuTakesPrecedence",
                                      {.corrected_rom_data = bytes::Bytes{4, 5, 6},
                                       .canceled_due_to_missing_module = true,
                                       .unknown_mcu_type = true},
                                      false,
                                      true}),
    [](const ::testing::TestParamInfo<UncorrectedCase>& info) { return std::string{info.param.name}; });

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

} // namespace
