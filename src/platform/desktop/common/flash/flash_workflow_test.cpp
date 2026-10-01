#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "src/backend/calibration/calibration_service.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

namespace fastecu::flash
{
namespace
{

FlashWorkflowRequest request(std::string protocol, FlashOperation operation = FlashOperation::Read)
{
    return {.operation = operation,
            .protocol = std::move(protocol),
            .mcu = "M32R_384KB_1block",
            .image = std::nullopt,
            .paths = {},
            .display_filename = "test.bin",
            .serial = nullptr};
}

FlashWorkflowRequest unisiaM32rWrite()
{
    auto input = request("sub_ecu_unisia_jecs_20", FlashOperation::Write);
    input.mcu = "M32R_128KB";
    input.image = bytes::Bytes(0x20000, 0xff);
    return input;
}

// Begin -> ApplyProgrammingVoltage -> attempt, all accepted.
std::unique_ptr<FlashWorkflow> unisiaM32rWriteAtAttempt()
{
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaM32rWrite());
    if (workflow == nullptr || std::get<FlashPromptStep>(workflow->next()).kind != FlashPromptKind::Begin)
    {
        return nullptr;
    }
    workflow->submit(FlashPromptResponse::Accept);
    if (std::get<FlashPromptStep>(workflow->next()).kind != FlashPromptKind::ApplyProgrammingVoltage)
    {
        return nullptr;
    }
    workflow->submit(FlashPromptResponse::Accept);
    if (!std::holds_alternative<FlashAttempt>(workflow->next()))
    {
        return nullptr;
    }
    return workflow;
}

using PromptArguments = std::vector<std::pair<std::string, std::string>>;

FlashWorkflowRequest unisiaBootmodeWrite(const config::ConfigPaths& paths)
{
    auto input = request("sub_ecu_unisia_jecs_20_bootmode", FlashOperation::Write);
    input.mcu = "M32R_128KB";
    input.image = bytes::Bytes(0x20000, 0xa5);
    input.paths = paths;
    return input;
}

// Begin -> ApplyBootModeVoltages -> kernel attempt, all accepted.
std::unique_ptr<FlashWorkflow> unisiaBootmodeAtKernelAttempt(const config::ConfigPaths& paths)
{
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(paths));
    if (workflow == nullptr || std::get<FlashPromptStep>(workflow->next()).kind != FlashPromptKind::Begin)
    {
        return nullptr;
    }
    workflow->submit(FlashPromptResponse::Accept);
    if (std::get<FlashPromptStep>(workflow->next()).kind != FlashPromptKind::ApplyBootModeVoltages)
    {
        return nullptr;
    }
    workflow->submit(FlashPromptResponse::Accept);
    if (!std::holds_alternative<FlashAttempt>(workflow->next()))
    {
        return nullptr;
    }
    return workflow;
}

PromptArguments bootmodeNotice(std::string outcome)
{
    return {{"outcome", std::move(outcome)}, {"external_vpp", "yes"}, {"power_off_advice", "no"}};
}

bool writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

std::optional<config::ConfigPaths> catalogPaths(const QTemporaryDir& directory, bool include_kernel_files = true)
{
    constexpr auto catalog = R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <protocols>
    <protocol name="sub_ecu_denso_mc68hc16y5_02" alias="wrx02">
      <ecu>Denso MC68HC16Y5</ecu><mcu>MC68HC16Y5</mcu>
      <kernel>catalog_mc68.bin</kernel><kernel_addr>0x20000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_mc68hc16y5_02_tpu" alias="wrx02-tpu">
      <ecu>Denso MC68HC16Y5</ecu><mcu>MC68HC16Y5_TPU</mcu>
      <kernel>catalog_tpu.bin</kernel><kernel_addr>0x20000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_mc68hc16y5_02_bdm">
      <ecu>Denso MC68HC16Y5</ecu><mcu>MC68HC16Y5</mcu>
      <kernel>catalog_mc68.bin</kernel><kernel_addr>0x20000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7055_02" alias="fxt02">
      <ecu>Denso SH7055</ecu><mcu>SH7055</mcu>
      <kernel>catalog_sh7055.bin</kernel><kernel_addr>0xFFFF6004</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7055_02_ecutek" alias="fxt02-ecutek">
      <ecu>Denso SH7055</ecu><mcu>SH7055</mcu>
      <kernel>catalog_sh7055.bin</kernel><kernel_addr>0xFFFF6004</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7055_densocan" alias="densocan-sh7055">
      <ecu>Denso SH7055</ecu><mcu>SH7055</mcu>
      <kernel>catalog_densocan.bin</kernel><kernel_addr>0xFFFF6004</kernel_addr>
    </protocol>
    <protocol name="sub_tcu_denso_sh7055_can" alias="tcu-sh7055">
      <ecu>Denso TCU SH7055</ecu><mcu>SH7055</mcu>
      <kernel>catalog_tcu_sh7055.bin</kernel><kernel_addr>0xFFFF9000</kernel_addr>
    </protocol>
    <protocol name="sub_tcu_denso_sh7058_can" alias="tcu-sh7058">
      <ecu>Denso TCU SH7058</ecu><mcu>SH7058</mcu>
      <kernel>catalog_tcu_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can" alias="subarucan">
      <ecu>Denso SH7058 petrol</ecu><mcu>SH7058</mcu>
      <kernel>catalog_petrol_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can_ecutek" alias="subarucan-ecutek">
      <ecu>Denso SH7058 petrol EcuTek</ecu><mcu>SH7058</mcu>
      <kernel>catalog_petrol_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can_ecutek_racerom" alias="subarucan-racerom">
      <ecu>Denso SH7058 petrol RaceRom</ecu><mcu>SH7058</mcu>
      <kernel>catalog_petrol_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can_ecutek_racerom_alt" alias="subarucan-racerom-alt">
      <ecu>Denso SH7058 petrol RaceRom alt</ecu><mcu>SH7058</mcu>
      <kernel>catalog_petrol_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can_cobb" alias="subarucan-cobb">
      <ecu>Denso SH7058 petrol Cobb</ecu><mcu>SH7058</mcu>
      <kernel>catalog_petrol_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_can_diesel" alias="subarucand">
      <ecu>Denso SH7058 diesel</ecu><mcu>SH7058d</mcu>
      <kernel>catalog_diesel_sh7058.bin</kernel><kernel_addr>0xFFFF4000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7059_can_diesel" alias="subarucand">
      <ecu>Denso SH7059 diesel</ecu><mcu>SH7059d</mcu>
      <kernel>catalog_diesel_sh7059.bin</kernel><kernel_addr>0xFFFEE000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7055_04" alias="sti04">
      <ecu>Denso SH7055</ecu><mcu>SH7055</mcu>
      <kernel>catalog_kline_sh7055.bin</kernel><kernel_addr>0xFFFF6004</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_ecutek" alias="sti05_ecutek">
      <ecu>Denso SH7058</ecu><mcu>SH7058</mcu>
      <kernel>catalog_kline_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_denso_sh7058_cobb" alias="sti05_cobb">
      <ecu>Denso SH7058</ecu><mcu>SH7058</mcu>
      <kernel>catalog_kline_sh7058.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
    </protocol>
    <protocol name="sub_ecu_unisia_jecs_20_bootmode">
      <ecu>WA12212920WWW</ecu><mcu>M32R_128KB</mcu>
      <kernel>catalog_uj20_bootmode.bin</kernel>
    </protocol>
    <protocol name="sub_ecu_unisia_jecs_30_bootmode">
      <ecu>WA12212930WWW</ecu><mcu>M32R_256KB</mcu>
      <kernel>catalog_uj30_bootmode.bin</kernel>
    </protocol>
  </protocols>
  <car_models>
    <car_model><make>Subaru</make><model>Impreza</model><version>WRX</version>
      <protocol>sub_ecu_denso_mc68hc16y5_02</protocol></car_model>
    <car_model><make>Subaru</make><model>Impreza</model><version>WRX TPU</version>
      <protocol>sub_ecu_denso_mc68hc16y5_02_tpu</protocol></car_model>
    <car_model><make>Subaru</make><model>Forester</model><version>XT</version>
      <protocol>sub_ecu_denso_sh7055_02</protocol></car_model>
  </car_models>
</config>)";

    const QString kernel_directory = directory.filePath("kernels");
    if (!QDir().mkpath(kernel_directory) || !writeFile(directory.filePath("protocols.cfg"), catalog))
    {
        return std::nullopt;
    }
    if (include_kernel_files)
    {
        if (!writeFile(kernel_directory + "/catalog_mc68.bin", QByteArray::fromHex("112233")) ||
            !writeFile(kernel_directory + "/catalog_tpu.bin", QByteArray::fromHex("445566")) ||
            !writeFile(kernel_directory + "/catalog_sh7055.bin", QByteArray::fromHex("aabbccdd")) ||
            !writeFile(kernel_directory + "/catalog_densocan.bin", QByteArray::fromHex("aabbccdd")) ||
            !writeFile(kernel_directory + "/catalog_tcu_sh7055.bin", QByteArray::fromHex("10203040")) ||
            !writeFile(kernel_directory + "/catalog_tcu_sh7058.bin", QByteArray::fromHex("50607080")) ||
            !writeFile(kernel_directory + "/catalog_petrol_sh7058.bin", QByteArray::fromHex("90a0b0c0")) ||
            !writeFile(kernel_directory + "/catalog_diesel_sh7058.bin", QByteArray::fromHex("d0e0f001")) ||
            !writeFile(kernel_directory + "/catalog_diesel_sh7059.bin", QByteArray::fromHex("d0e0f002")) ||
            !writeFile(kernel_directory + "/catalog_kline_sh7055.bin", QByteArray::fromHex("aabbccdd")) ||
            !writeFile(kernel_directory + "/catalog_kline_sh7058.bin", QByteArray::fromHex("01020304")) ||
            !writeFile(kernel_directory + "/catalog_uj20_bootmode.bin", QByteArray::fromHex("0102030405")) ||
            !writeFile(kernel_directory + "/catalog_uj30_bootmode.bin", QByteArray::fromHex("0607")))
        {
            return std::nullopt;
        }
    }
    config::ConfigPaths paths;
    paths.protocols_file = directory.filePath("protocols.cfg").toStdString();
    paths.kernel_files_directory = (kernel_directory + "/").toStdString();
    return paths;
}

std::unique_ptr<SerialPortActions> recordingSerial(FakeBackend **fake)
{
    auto serial = std::make_unique<SerialPortActions>(
        [fake]() -> SerialBackend *
        {
            *fake = new NiceFakeBackend;
            return *fake;
        });
    if (!serial->set_add_ssm_header(false) || *fake == nullptr)
    {
        return nullptr;
    }
    return serial;
}

void expectCanTransportSetup(FakeBackend& fake, bool reset, std::uint32_t source, std::uint32_t destination)
{
    ::testing::InSequence sequence;
    if (reset)
    {
        EXPECT_CALL(fake, reset_connection()).WillOnce(::testing::Return());
    }
    EXPECT_CALL(fake, set_is_iso15765_connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, set_is_can_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, set_is_iso14230_connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, set_is_29_bit_id(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, set_can_speed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, set_can_source_address(source)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, set_can_destination_address(destination)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, set_iso15765_source_address(source)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, set_iso15765_destination_address(destination)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, set_add_iso14230_header(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, open_serial_port()).WillOnce(::testing::Return(QStringLiteral("COM3")));
}

void expectNoBackendIo(FakeBackend& fake)
{
    EXPECT_CALL(fake, is_serial_port_open()).Times(0);
    EXPECT_CALL(fake, reset_connection()).Times(0);
    EXPECT_CALL(fake, change_port_speed(::testing::_)).Times(0);
    EXPECT_CALL(fake, open_serial_port()).Times(0);
    EXPECT_CALL(fake, read_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(fake, write_serial_data(::testing::_)).Times(0);
    EXPECT_CALL(fake, write_serial_data_echo_check(::testing::_)).Times(0);
    EXPECT_CALL(fake, read_vbatt()).Times(0);
}

TEST(FlashWorkflowTest, recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone)
{
    static constexpr auto portable = std::to_array<const char *>({"mitsu_ecu_m32r_can",
                                                                  "mitsu_ecu_m32r_can_vendor_ext",
                                                                  "mitsu_ecu_m32r_can_512kb",
                                                                  "mitsu_ecu_m32r_can_vendor_ext_512kb",
                                                                  "sub_ecu_mitsu_m32r_kline",
                                                                  "sub_ecu_hitachi_m32r_kline",
                                                                  "sub_ecu_hitachi_m32r_kline_recovery",
                                                                  "sub_ecu_eeprom_denso_sh7055_kline",
                                                                  "sub_ecu_eeprom_denso_sh7058_kline",
                                                                  "sub_ecu_eeprom_denso_sh7055_densocan",
                                                                  "sub_ecu_eeprom_denso_sh7058_densocan",
                                                                  "sub_ecu_eeprom_denso_sh7058_can",
                                                                  "sub_ecu_eeprom_denso_sh7058_can_diesel",
                                                                  "sub_ecu_hitachi_m32r_can",
                                                                  "sub_tcu_hitachi_m32r_kline",
                                                                  "sub_ecu_unisia_jecs_m3779x",
                                                                  "sub_ecu_unisia_jecs_m3775x",
                                                                  "sub_tcu_hitachi_m32r_can",
                                                                  "sub_tcu_cvt_hitachi_m32r_can",
                                                                  "sub_tcu_cvt_mitsu_mh8111_can",
                                                                  "sub_tcu_cvt_mitsu_mh8104_can",
                                                                  "sub_ecu_denso_1n83m_1_5m_can",
                                                                  "sub_ecu_denso_sh72531_can",
                                                                  "sub_ecu_denso_sh72543_can_diesel",
                                                                  "sub_ecu_denso_sh7058_can_diesel",
                                                                  "sub_ecu_denso_sh7059_can_diesel",
                                                                  "sub_ecu_denso_1n83m_4m_can",
                                                                  "sub_ecu_denso_mc68hc16y5_02_bdm",
                                                                  "sub_ecu_unisia_jecs_20",
                                                                  "sub_ecu_unisia_jecs_30",
                                                                  "sub_ecu_unisia_jecs_40",
                                                                  "sub_ecu_unisia_jecs_70"});
    for (const char *protocol : portable)
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr) << protocol;
    }
}

TEST(FlashWorkflowTest, invalidColtSuffixIsRecognizedButFailsPreflight)
{
    auto workflow = FlashWorkflowFactory::tryCreate(request("mitsu_ecu_m32r_can_typo"));
    ASSERT_TRUE(workflow != nullptr);
    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

TEST(FlashWorkflowTest, preflightPrecedesPromptsAndDeclineCancels)
{
    auto invalid = request("mitsu_ecu_m32r_can", FlashOperation::TestWrite);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(invalid));
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(workflow->next()));

    workflow = FlashWorkflowFactory::tryCreate(request("mitsu_ecu_m32r_can"));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

TEST(FlashWorkflowTest, successfulReadBytesAreAcceptedAutomatically)
{
    auto workflow = FlashWorkflowFactory::tryCreate(request("mitsu_ecu_m32r_can"));
    (void)workflow->next();
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1, 2, 3}});
    auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({1, 2, 3}));
}

TEST(FlashWorkflowTest, unisiaJecsRoutesOnlyExactProtocolMcuPairs)
{
    static constexpr auto pairs = std::to_array<std::pair<const char *, const char *>>({
        {"sub_ecu_unisia_jecs_m3779x", "M3779x"},
        {"sub_ecu_unisia_jecs_m3775x", "M3775x"},
    });

    for (const auto& [protocol, mcu] : pairs)
    {
        auto input = request(protocol);
        input.mcu = mcu;
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << protocol;
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        ASSERT_EQ(plan.family(), FlashFamily::SubaruUnisiaJecs);
        ASSERT_EQ(plan.transport(), TransportKind::Kline);
        ASSERT_EQ(plan.target_id(), protocol);
        ASSERT_EQ(plan.mcu_name(), mcu);
    }

    ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request("sub_ecu_unisia_jecs_m3779x_suffix")) == nullptr);
    ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request("sub_ecu_unisia_jecs_m3775x_suffix")) == nullptr);
}

TEST(FlashWorkflowTest, unisiaJecsCrossPairsFailBeforeAttempt)
{
    static constexpr auto cross_pairs = std::to_array<std::pair<const char *, const char *>>({
        {"sub_ecu_unisia_jecs_m3779x", "M3775x"},
        {"sub_ecu_unisia_jecs_m3775x", "M3779x"},
    });

    for (const auto& [protocol, mcu] : cross_pairs)
    {
        auto input = request(protocol);
        input.mcu = mcu;
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << protocol;
        auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
    }
}

TEST(FlashWorkflowTest, subaruMitsuPropagatesRomId)
{
    auto input = request("sub_ecu_mitsu_m32r_kline");
    input.mcu = "M32R_512KB_4blocks";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0xff, 0x12}, .rom_id = "123456789A_"});
    auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
}

TEST(FlashWorkflowTest, subaruHitachiRoutesBothModesAndPropagatesReadResult)
{
    for (const char *protocol : {"sub_ecu_hitachi_m32r_kline", "sub_ecu_hitachi_m32r_kline_recovery"})
    {
        auto input = request(protocol);
        input.mcu = "M32R_512KB_1block";
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
        workflow->submit(
            FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
        auto done = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
        ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5a}));
        ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
    }
}

TEST(FlashWorkflowTest, routesTcuHitachiM32rKlineReadOnly)
{
    auto input = request("sub_tcu_hitachi_m32r_kline");
    input.mcu = "M32R_512KB";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5a}));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));

    // Write is rejected by the plan builder (the family is read-only), so the
    // workflow's very first step must be a failure rather than a prompt or an
    // attempt -- the legacy path silently "succeeded" while writing nothing.
    auto write_request = request("sub_tcu_hitachi_m32r_kline");
    write_request.mcu = "M32R_512KB";
    write_request.operation = FlashOperation::Write;
    write_request.image = bytes::Bytes(0x80000, 0x00);
    auto write_workflow = FlashWorkflowFactory::tryCreate(std::move(write_request));
    ASSERT_TRUE(write_workflow != nullptr);
    const auto write_step = write_workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(write_step));
    ASSERT_EQ(std::get<FlashFailureStep>(write_step).error.kind, ErrorKind::Unsupported);
}

TEST(FlashWorkflowTest, routesTcuHitachiM32rCanReadAndWriteRejectsTestWrite)
{
    // The default request() helper's MCU ("M32R_384KB_1block") is not this
    // family's kMcu ("M32R_512KB"); build_subaru_tcu_hitachi_m32r_can_plan's
    // identity check would reject every operation on it with InvalidConfig,
    // which would make the Read and Write assertions below fail for an
    // unrelated reason and would mask the TestWrite assertion behind the
    // same wrong-MCU failure instead of the Unsupported this test is meant to
    // pin. Setting the real MCU here is what makes the three assertions below
    // test routing, not identity validation.
    constexpr auto kMcu = "M32R_512KB";
    constexpr auto kProtocol = "sub_tcu_hitachi_m32r_can";

    // Read routes to an attempt bound to the CAN executor and transport, and
    // a successful attempt result is propagated through to completion.
    auto read_input = request(kProtocol);
    read_input.mcu = kMcu;
    auto read_workflow = FlashWorkflowFactory::tryCreate(std::move(read_input));
    ASSERT_TRUE(read_workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(read_workflow->next()).kind, FlashPromptKind::Begin);
    read_workflow->submit(FlashPromptResponse::Accept);
    const auto read_step = read_workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(read_step));
    const FlashPlan& read_plan = std::get<FlashAttempt>(read_step).attempt->plan();
    ASSERT_EQ(read_plan.target_id(), std::string_view(kProtocol));
    ASSERT_TRUE(read_plan.operation() == FlashOperation::Read);
    ASSERT_EQ(read_plan.transport(), TransportKind::CanIso15765);
    read_workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}});
    const auto read_done = read_workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(read_done));
    ASSERT_EQ(std::get<FlashCompletedStep>(read_done).outcome, FlashWorkflowOutcome::Succeeded);
    ASSERT_EQ(std::get<FlashCompletedStep>(read_done).accepted_read_bytes, bytes::Bytes({0x5a}));

    // TestWrite is rejected here via the plan-builder check (deliberate
    // divergence 1: legacy reflash_block ignored its test_write_arg and
    // performed a real erase and flash write, so there is no dry run to
    // route to). This family has exactly two independent TestWrite checks --
    // this plan-builder one, and the plan-validator one the executor calls
    // on every plan -- not four; this workflow and the executor's entry
    // points are consumers of those two, not additional checks of their own.
    // The workflow's very first step must be a failure, not a prompt -- the
    // legacy path silently "succeeded" while performing a real write.
    auto test_write_input = request(kProtocol, FlashOperation::TestWrite);
    test_write_input.mcu = kMcu;
    test_write_input.image = bytes::Bytes(0x80000, 0xa5);
    auto test_write_workflow = FlashWorkflowFactory::tryCreate(std::move(test_write_input));
    ASSERT_TRUE(test_write_workflow != nullptr);
    const auto test_write_step = test_write_workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(test_write_step));
    ASSERT_EQ(std::get<FlashFailureStep>(test_write_step).error.kind, ErrorKind::Unsupported);

    // Write, unlike the K-Line sibling, is supported by this family and
    // routes all the way to an attempt bound to the CAN executor/transport.
    auto write_input = request(kProtocol, FlashOperation::Write);
    write_input.mcu = kMcu;
    write_input.image = bytes::Bytes(0x80000, 0xa5);
    auto write_workflow = FlashWorkflowFactory::tryCreate(std::move(write_input));
    ASSERT_TRUE(write_workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(write_workflow->next()).kind, FlashPromptKind::Begin);
    write_workflow->submit(FlashPromptResponse::Accept);
    const auto write_step = write_workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(write_step));
    const FlashPlan& write_plan = std::get<FlashAttempt>(write_step).attempt->plan();
    ASSERT_EQ(write_plan.target_id(), std::string_view(kProtocol));
    ASSERT_TRUE(write_plan.operation() == FlashOperation::Write);
    ASSERT_EQ(write_plan.transport(), TransportKind::CanIso15765);
}

TEST(FlashWorkflowTest, routesSh72543rAliasesAndPreservesImageAndIdentity)
{
    for (const char *protocol : {"sub_ecu_hitachi_sh72543r_can", "sub_ecu_hitachi_sh72543r_can_recovery"})
    {
        for (auto operation : {FlashOperation::Read, FlashOperation::Write})
        {
            auto input = request(protocol, operation);
            input.mcu = "SH72543R";
            if (operation == FlashOperation::Write)
            {
                input.image = bytes::Bytes(0x200000, 0xa5);
            }
            auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
            ASSERT_TRUE(workflow);
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
            workflow->submit(FlashPromptResponse::Accept);
            auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
            const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
            ASSERT_EQ(plan.family(), FlashFamily::SubaruHitachiSh72543rCan);
            ASSERT_EQ(plan.target_id(), std::string_view(protocol));
            ASSERT_EQ(plan.transport(), TransportKind::CanIso15765);
            ASSERT_EQ(plan.transfer_region().start, operation == FlashOperation::Read ? 0U : 0x6000U);
            if (operation == FlashOperation::Write)
            {
                ASSERT_EQ(*plan.image(), bytes::Bytes(0x200000, 0xa5));
            }
            workflow->submit(FlashAttemptResult{
                .success = true,
                .read_bytes = operation == FlashOperation::Read ? std::optional{bytes::Bytes{1, 2, 3}} : std::nullopt,
                .rom_id =
                    operation == FlashOperation::Read ? std::optional<std::string>{"CAL_1122334455_"} : std::nullopt});
            auto done = std::get<FlashCompletedStep>(workflow->next());
            ASSERT_EQ(done.outcome, FlashWorkflowOutcome::Succeeded);
            if (operation == FlashOperation::Read)
            {
                ASSERT_EQ(done.accepted_read_bytes, bytes::Bytes({1, 2, 3}));
                ASSERT_EQ(done.rom_id, std::string("CAL_1122334455_"));
            }
            else
            {
                ASSERT_TRUE(!done.accepted_read_bytes);
                ASSERT_TRUE(!done.rom_id);
            }
        }
    }
    ASSERT_TRUE(!FlashWorkflowFactory::tryCreate(request("sub_ecu_hitachi_sh72543r_can_recovery_typo")));
    ASSERT_TRUE(!FlashWorkflowFactory::tryCreate(request("sub_ecu_hitachi_sh72543r_can_typo")));
}
TEST(FlashWorkflowTest, routesSh7058ReadAndWriteWithPreTransportPrompts)
{
    ASSERT_TRUE(!FlashWorkflowFactory::tryCreate(request("sub_ecu_hitachi_sh7058_can_extra")));
    for (const auto operation : {FlashOperation::Read, FlashOperation::Write})
    {
        auto input = request("sub_ecu_hitachi_sh7058_can", operation);
        input.mcu = "SH7058_1block";
        if (operation == FlashOperation::Write)
        {
            input.image = bytes::Bytes(0x100000, 0x5a);
        }
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow);
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        if (operation == FlashOperation::Read)
        {
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ConfirmSh7058Read);
            workflow->submit(FlashPromptResponse::Accept);
        }
        auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        ASSERT_EQ(plan.family(), FlashFamily::SubaruHitachiSh7058);
        ASSERT_EQ(plan.transport(),
                  operation == FlashOperation::Read ? TransportKind::Kline : TransportKind::CanIso15765);
    }
    auto input = request("sub_ecu_hitachi_sh7058_can");
    input.mcu = "SH7058_1block";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    workflow->submit(FlashPromptResponse::Accept);
    workflow->submit(FlashPromptResponse::Decline);
    ASSERT_EQ(std::get<FlashCompletedStep>(workflow->next()).outcome, FlashWorkflowOutcome::Cancelled);
}
TEST(FlashWorkflowTest, sh72543rRejectsPreflightAndDeclinedBegin)
{
    for (const char *protocol : {"sub_ecu_hitachi_sh72543r_can", "sub_ecu_hitachi_sh72543r_can_recovery"})
    {
        for (int fault = 0; fault < 3; ++fault)
        {
            auto input = request(protocol, fault == 0 ? FlashOperation::TestWrite : FlashOperation::Write);
            input.mcu = fault == 1 ? "SH72543d" : "SH72543R";
            input.image = bytes::Bytes(fault == 2 ? 16 : 0x200000);
            auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
            ASSERT_TRUE(workflow);
            auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind,
                      fault == 0 ? ErrorKind::Unsupported : ErrorKind::InvalidConfig);
        }
        auto input = request(protocol);
        input.mcu = "SH72543R";
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow);
        ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(workflow->next()));
        workflow->submit(FlashPromptResponse::Decline);
        auto done = std::get<FlashCompletedStep>(workflow->next());
        ASSERT_EQ(done.outcome, FlashWorkflowOutcome::Cancelled);
        ASSERT_TRUE(!done.accepted_read_bytes);
    }
}
TEST(FlashWorkflowTest, sh72543rPropagatesFailureAndAbsentIdentity)
{
    for (int outcome = 0; outcome < 3; ++outcome)
    {
        auto input = request("sub_ecu_hitachi_sh72543r_can");
        input.mcu = "SH72543R";
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow);
        workflow->submit(FlashPromptResponse::Accept);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
        workflow->submit(
            FlashAttemptResult{.success = outcome == 0,
                               .error_kind = outcome == 1 ? ErrorKind::Disconnected : ErrorKind::Cancelled,
                               .error_detail = "lost adapter",
                               .read_bytes = outcome == 0 ? std::optional{bytes::Bytes{4, 5}} : std::nullopt});
        auto step = workflow->next();
        if (outcome == 1)
        {
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Disconnected);
        }
        else
        {
            auto done = std::get<FlashCompletedStep>(step);
            ASSERT_TRUE(!done.rom_id);
            ASSERT_EQ(done.outcome, outcome == 0 ? FlashWorkflowOutcome::Succeeded : FlashWorkflowOutcome::Cancelled);
        }
    }
}

TEST(FlashWorkflowTest, coltWriteUsesColtSpecificSafetyPrompts)
{
    auto write = request("mitsu_ecu_m32r_can", FlashOperation::Write);
    write.image = bytes::Bytes(0x60000);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(write));

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ColtEraseTrigger);
}

TEST(FlashWorkflowTest, mc68BdmReadRoutesThroughBeginToAttempt)
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm");
    input.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    ASSERT_EQ(plan.family(), FlashFamily::SubaruDensoMc68hc16y5_02Bdm);
    ASSERT_EQ(plan.transport(), TransportKind::Kline);
    ASSERT_EQ(plan.transfer_region(), (MemoryRegion{0, 0x30000}));
    ASSERT_TRUE(!plan.image().has_value());
}

TEST(FlashWorkflowTest, mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    input.image = bytes::Bytes(0x30000, 0x5a);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        FAIL() << failure->error.detail.c_str();
    }
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ConfirmBdmKernelBootstrap);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    bytes::Bytes expected(0x20, 0x00);
    expected[0] = 0x11;
    expected[1] = 0x22;
    expected[2] = 0x33;
    ASSERT_EQ(plan.image(), std::optional<bytes::Bytes>(expected));
    ASSERT_EQ(plan.transfer_region(), (MemoryRegion{0x20000, 0x20}));
    ASSERT_TRUE(!plan.kernel().has_value());
}

TEST(FlashWorkflowTest, mc68BdmDeclinedBootstrapConfirmationCancels)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ConfirmBdmKernelBootstrap);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

TEST(FlashWorkflowTest, mc68BdmDeclinedBeginCancels)
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm");
    input.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

TEST(FlashWorkflowTest, mc68BdmTestWriteFailsBeforeAnyPrompt)
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::TestWrite);
    input.mcu = "MC68HC16Y5";
    input.image = bytes::Bytes(0x30000, 0x5a);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    const auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

TEST(FlashWorkflowTest, mc68BdmPrefixLookalikeStaysOffTheKlineFamily)
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm_x");
    input.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    const auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

TEST(FlashWorkflowTest, mc68TpuProtocolIsClaimedByPortableRoute)
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_tpu");
    input.mcu = "MC68HC16Y5_TPU";
    ASSERT_TRUE(FlashWorkflowFactory::tryCreate(std::move(input)) != nullptr);
}

TEST(FlashWorkflowTest, mc68Revision04IsClaimedButPlanBuildFails)
{
    auto input = request("sub_ecu_denso_mc68hc16y5_04");
    input.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    const auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

TEST(FlashWorkflowTest, sh7055ProtocolIsClaimedByPortableRoute)
{
    auto input = request("sub_ecu_denso_sh7055_02");
    input.mcu = "SH7055";
    ASSERT_TRUE(FlashWorkflowFactory::tryCreate(std::move(input)) != nullptr);
}

TEST(FlashWorkflowTest, densoCanRoutesOnlyTheFiveExactProtocols)
{
    constexpr auto kProtocols = std::to_array<const char *>({
        "sub_ecu_denso_sh7055_densocan",
        "sub_ecu_denso_sh7058_densocan",
        "sub_ecu_denso_sh7058s_densocan",
        "sub_ecu_denso_sh7058s_diesel_densocan",
        "sub_ecu_denso_sh7059_diesel_densocan",
    });
    for (const char *protocol : kProtocols)
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr) << protocol;
    }
    for (const char *near_miss : {"sub_ecu_denso_sh7058_densocan_extra", "future_densocan"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr) << near_miss;
    }
}

TEST(FlashWorkflowTest, densoCanResolvesKernelPromptsAndPropagatesAttemptResult)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_sh7055_densocan");
    input.mcu = "SH7055";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    ASSERT_TRUE(QFile::remove(QString::fromStdString(paths->protocols_file)));
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_densocan.bin")));
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::CycleIgnition);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    ASSERT_EQ(plan.transport(), TransportKind::CanRawIso15765);
    ASSERT_TRUE(plan.kernel().has_value());
    ASSERT_EQ(plan.kernel()->bytes, bytes::Bytes({0xaa, 0xbb, 0xcc, 0xdd}));

    workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
    const auto& done = std::get<FlashCompletedStep>(step);
    ASSERT_EQ(done.outcome, FlashWorkflowOutcome::Succeeded);
    ASSERT_EQ(done.accepted_read_bytes, bytes::Bytes({0x5a}));
    ASSERT_EQ(done.rom_id, std::string("123456789A_"));
}

TEST(FlashWorkflowTest, densoCanPreflightAndDeclinedPromptsStopBeforeAttempt)
{
    auto missing_catalog = request("sub_ecu_denso_sh7055_densocan");
    missing_catalog.mcu = "SH7055";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(missing_catalog));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(workflow->next()));

    for (const bool decline_begin : {true, false})
    {
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        auto input = request("sub_ecu_denso_sh7055_densocan");
        input.mcu = "SH7055";
        const auto paths = catalogPaths(directory);
        ASSERT_TRUE(paths.has_value());
        input.paths = *paths;
        workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        if (decline_begin)
        {
            workflow->submit(FlashPromptResponse::Decline);
        }
        else
        {
            workflow->submit(FlashPromptResponse::Accept);
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::CycleIgnition);
            workflow->submit(FlashPromptResponse::Decline);
        }
        const auto done = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
        ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
    }
}

TEST(FlashWorkflowTest, petrolRoutesOnlyTheFiveExactProtocols)
{
    constexpr auto kProtocols = std::to_array<const char *>({
        "sub_ecu_denso_sh7058_can",
        "sub_ecu_denso_sh7058_can_ecutek",
        "sub_ecu_denso_sh7058_can_ecutek_racerom",
        "sub_ecu_denso_sh7058_can_ecutek_racerom_alt",
        "sub_ecu_denso_sh7058_can_cobb",
    });
    for (const char *protocol : kProtocols)
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr) << protocol;
    }
    for (const char *near_miss : {
             "sub_ecu_denso_sh7058_can_future",
             "sub_ecu_denso_sh7058_can_ecutek_extra",
             "sub_ecu_denso_sh7058_can_cobb_typo",
         })
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr) << near_miss;
    }
}

TEST(FlashWorkflowTest, petrolSupportedOperationsResolveSecurityAndCatalogKernel)
{
    struct Case
    {
        const char *protocol;
        SubaruDensoSh7058CanSecurity security;
        FlashOperation operation;
    };
    const std::array cases{
        Case{"sub_ecu_denso_sh7058_can", SubaruDensoSh7058CanSecurity::Stock, FlashOperation::Read},
        Case{"sub_ecu_denso_sh7058_can_ecutek", SubaruDensoSh7058CanSecurity::EcuTek, FlashOperation::TestWrite},
        Case{"sub_ecu_denso_sh7058_can_ecutek_racerom", SubaruDensoSh7058CanSecurity::RaceRom, FlashOperation::Write},
        Case{"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", SubaruDensoSh7058CanSecurity::RaceRomAlt,
             FlashOperation::Read},
        Case{"sub_ecu_denso_sh7058_can_cobb", SubaruDensoSh7058CanSecurity::Cobb, FlashOperation::TestWrite},
    };

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const Case& test : cases)
    {
        auto input = request(test.protocol, test.operation);
        input.mcu = "SH7058";
        input.paths = *paths;
        if (test.operation != FlashOperation::Read)
        {
            input.image = bytes::Bytes(0x00100000, bytes::Byte{0xA5});
        }
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << test.protocol;

        auto step = workflow->next();
        if (const auto *failure = std::get_if<FlashFailureStep>(&step))
        {
            FAIL() << failure->error.detail.c_str();
        }
        ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
        ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->plan();
        ASSERT_EQ(plan.family(), FlashFamily::SubaruDensoSh7058Can);
        ASSERT_EQ(plan.transport(), TransportKind::CanIso15765);
        ASSERT_EQ(plan.target_id(), std::string_view(test.protocol));
        ASSERT_EQ(plan.mcu_name(), std::string_view("SH7058"));
        ASSERT_EQ(plan.operation(), test.operation);
        ASSERT_TRUE(plan.confirmations().empty());
        ASSERT_TRUE(plan.kernel().has_value());
        ASSERT_EQ(plan.kernel()->load_address, 0xFFFF3000U);
        ASSERT_EQ(plan.kernel()->bytes, bytes::Bytes({0x90, 0xA0, 0xB0, 0xC0}));
        const auto *family_plan = std::get_if<SubaruDensoSh7058CanPlan>(&plan.family_plan());
        ASSERT_TRUE(family_plan != nullptr);
        ASSERT_EQ(family_plan->request_id, 0x7E0U);
        ASSERT_EQ(family_plan->response_id, 0x7E8U);
        ASSERT_EQ(family_plan->bitrate, 500000);
        ASSERT_TRUE(!family_plan->extended_id);
        ASSERT_EQ(family_plan->security, test.security);
    }
}

TEST(FlashWorkflowTest, petrolSuccessfulReadPropagatesBytesAndRomId)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_sh7058_can");
    input.mcu = "SH7058";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5A, 0xA5}, .rom_id = "CALID_123456789A_"});
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5A, 0xA5}));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("CALID_123456789A_"));
}

TEST(FlashWorkflowTest, petrolReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectCanTransportSetup(*fake, true, 2016, 2024);

    auto input = request("sub_ecu_denso_sh7058_can");
    input.mcu = "SH7058";
    input.paths = *paths;
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    // Resolution happened before Begin; removing the catalog and kernel now
    // must not affect the already-bound attempt.
    ASSERT_TRUE(QFile::remove(QString::fromStdString(paths->protocols_file)));
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_petrol_sh7058.bin")));

    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->plan();
    ASSERT_EQ(plan.family(), FlashFamily::SubaruDensoSh7058Can);
    ASSERT_EQ(plan.transport(), TransportKind::CanIso15765);
    ASSERT_EQ(plan.target_id(), std::string_view("sub_ecu_denso_sh7058_can"));
    ASSERT_TRUE(plan.kernel().has_value());
    ASSERT_EQ(plan.kernel()->bytes, bytes::Bytes({0x90, 0xA0, 0xB0, 0xC0}));

    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(5);
    NullEventSink events;
    const auto result = attempt.attempt->run(*attempt.clock, cancellation, events);
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST(FlashWorkflowTest, dieselRoutesOnlyTheTwoExactProtocols)
{
    for (const char *protocol : {"sub_ecu_denso_sh7058_can_diesel", "sub_ecu_denso_sh7059_can_diesel"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr) << protocol;
    }
    for (const char *near_miss : {"sub_ecu_denso_sh7058_can_diesel_future", "sub_ecu_denso_sh7059_can_diesel_extra",
                                  "sub_ecu_denso_sh7058_can_diesel_typo", "sub_ecu_denso_sh7058_can_diesel_ecutek"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr) << near_miss;
    }
}

TEST(FlashWorkflowTest, dieselSupportedOperationsResolveGenerationCatalogKernels)
{
    struct Case
    {
        const char *protocol;
        const char *mcu;
        FlashOperation operation;
        std::size_t rom_size;
        std::uint32_t kernel_address;
        QByteArray kernel_bytes;
    };
    const std::array cases{
        Case{"sub_ecu_denso_sh7058_can_diesel", "SH7058d", FlashOperation::Read, 0x00100000, 0xFFFF4000,
             QByteArray::fromHex("d0e0f001")},
        Case{"sub_ecu_denso_sh7058_can_diesel", "SH7058d", FlashOperation::TestWrite, 0x00100000, 0xFFFF4000,
             QByteArray::fromHex("d0e0f001")},
        Case{"sub_ecu_denso_sh7058_can_diesel", "SH7058d", FlashOperation::Write, 0x00100000, 0xFFFF4000,
             QByteArray::fromHex("d0e0f001")},
        Case{"sub_ecu_denso_sh7059_can_diesel", "SH7059d", FlashOperation::Read, 0x00180000, 0xFFFEE000,
             QByteArray::fromHex("d0e0f002")},
        Case{"sub_ecu_denso_sh7059_can_diesel", "SH7059d", FlashOperation::TestWrite, 0x00180000, 0xFFFEE000,
             QByteArray::fromHex("d0e0f002")},
        Case{"sub_ecu_denso_sh7059_can_diesel", "SH7059d", FlashOperation::Write, 0x00180000, 0xFFFEE000,
             QByteArray::fromHex("d0e0f002")},
    };

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    for (const Case& test : cases)
    {
        auto input = request(test.protocol, test.operation);
        input.mcu = test.mcu;
        input.paths = *paths;
        if (test.operation != FlashOperation::Read)
        {
            input.image = bytes::Bytes(test.rom_size, bytes::Byte{0xA5});
        }
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << test.protocol;
        auto step = workflow->next();
        if (const auto *failure = std::get_if<FlashFailureStep>(&step))
        {
            FAIL() << failure->error.detail.c_str();
        }
        ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
        ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->plan();
        ASSERT_EQ(plan.family(), FlashFamily::SubaruDensoSh7058CanDiesel);
        ASSERT_EQ(plan.transport(), TransportKind::CanIso15765);
        ASSERT_EQ(plan.target_id(), std::string_view(test.protocol));
        ASSERT_EQ(plan.mcu_name(), std::string_view(test.mcu));
        ASSERT_EQ(plan.operation(), test.operation);
        ASSERT_EQ(plan.transfer_region().length, static_cast<std::uint32_t>(test.rom_size));
        ASSERT_TRUE(plan.confirmations().empty());
        ASSERT_TRUE(plan.kernel().has_value());
        ASSERT_EQ(plan.kernel()->load_address, test.kernel_address);
        ASSERT_EQ(QByteArray(reinterpret_cast<const char *>(plan.kernel()->bytes.data()),
                             static_cast<int>(plan.kernel()->bytes.size())),
                  test.kernel_bytes);
        const auto *family_plan = std::get_if<SubaruDensoSh7058CanDieselPlan>(&plan.family_plan());
        ASSERT_TRUE(family_plan != nullptr);
        ASSERT_EQ(family_plan->request_id, 0x7E0U);
        ASSERT_EQ(family_plan->response_id, 0x7E8U);
        ASSERT_EQ(family_plan->bitrate, 500000);
        ASSERT_TRUE(!family_plan->extended_id);
    }
}

TEST(FlashWorkflowTest, dieselSuccessfulReadPropagatesKernelSnapshotBytesAndRomId)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_sh7059_can_diesel");
    input.mcu = "SH7059d";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    ASSERT_TRUE(QFile::remove(QString::fromStdString(paths->protocols_file)));
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_diesel_sh7059.bin")));
    workflow->submit(FlashPromptResponse::Accept);
    const auto attempt_step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(attempt_step));
    const FlashPlan& snapshot = std::get<FlashAttempt>(attempt_step).attempt->plan();
    ASSERT_TRUE(snapshot.kernel().has_value());
    ASSERT_EQ(snapshot.kernel()->load_address, 0xFFFEE000U);
    ASSERT_EQ(snapshot.kernel()->bytes, bytes::Bytes({0xD0, 0xE0, 0xF0, 0x02}));

    workflow->submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0xD1, 0xE5}, .rom_id = "DIESEL_CAL_ECU_"});
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0xD1, 0xE5}));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("DIESEL_CAL_ECU_"));
}

TEST(FlashWorkflowTest, dieselReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectCanTransportSetup(*fake, true, 2016, 2024);

    auto input = request("sub_ecu_denso_sh7059_can_diesel");
    input.mcu = "SH7059d";
    input.paths = *paths;
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);

    // The workflow owns resolved catalog data before Begin; this also proves
    // the Diesel route is a real DesktopCan attempt rather than a legacy
    // MainWindow branch.
    ASSERT_TRUE(QFile::remove(QString::fromStdString(paths->protocols_file)));
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_diesel_sh7059.bin")));
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->plan();
    ASSERT_EQ(plan.family(), FlashFamily::SubaruDensoSh7058CanDiesel);
    ASSERT_EQ(plan.transport(), TransportKind::CanIso15765);
    ASSERT_EQ(plan.target_id(), std::string_view("sub_ecu_denso_sh7059_can_diesel"));
    ASSERT_EQ(plan.mcu_name(), std::string_view("SH7059d"));
    ASSERT_TRUE(plan.kernel().has_value());
    ASSERT_EQ(plan.kernel()->load_address, 0xFFFEE000U);
    ASSERT_EQ(plan.kernel()->bytes, bytes::Bytes({0xD0, 0xE0, 0xF0, 0x02}));

    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(53);
    NullEventSink events;
    const auto result = attempt.attempt->run(*attempt.clock, cancellation, events);
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST(FlashWorkflowTest, tcuRoutesOnlyTheTwoExactProtocols)
{
    for (const char *protocol : {"sub_tcu_denso_sh7055_can", "sub_tcu_denso_sh7058_can"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr) << protocol;
    }
    for (const char *near_miss :
         {"sub_tcu_denso_sh7055_can_future", "sub_tcu_denso_sh7058_can_typo", "sub_tcu_denso_sh7058_can_extra"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr) << near_miss;
    }
}

TEST(FlashWorkflowTest, tcuSupportedOperationsResolveTheirCatalogKernelAndReachAttempt)
{
    struct Case
    {
        const char *protocol;
        const char *mcu;
        FlashOperation operation;
        std::size_t image_size;
        std::uint32_t kernel_address;
        bytes::Bytes kernel_bytes;
    };
    const std::array cases{
        Case{"sub_tcu_denso_sh7055_can", "SH7055", FlashOperation::Read, 0, 0xFFFF9000, {0x10, 0x20, 0x30, 0x40}},
        Case{"sub_tcu_denso_sh7058_can", "SH7058", FlashOperation::Read, 0, 0xFFFF3000, {0x50, 0x60, 0x70, 0x80}},
        Case{"sub_tcu_denso_sh7058_can",
             "SH7058",
             FlashOperation::Write,
             0x100000,
             0xFFFF3000,
             {0x50, 0x60, 0x70, 0x80}},
    };

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const Case& test : cases)
    {
        auto input = request(test.protocol, test.operation);
        input.mcu = test.mcu;
        input.paths = *paths;
        if (test.image_size != 0)
        {
            input.image = bytes::Bytes(test.image_size, 0xa5);
        }
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << test.protocol;

        auto step = workflow->next();
        if (const auto *failure = std::get_if<FlashFailureStep>(&step))
        {
            FAIL() << failure->error.detail.c_str();
        }
        ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
        ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->plan();
        ASSERT_EQ(plan.target_id(), std::string_view(test.protocol));
        ASSERT_TRUE(plan.operation() == test.operation);
        ASSERT_EQ(plan.transport(), TransportKind::CanIso15765);
        ASSERT_TRUE(plan.kernel().has_value());
        ASSERT_EQ(plan.kernel()->load_address, test.kernel_address);
        ASSERT_EQ(plan.kernel()->bytes, test.kernel_bytes);
    }
}

TEST(FlashWorkflowTest, tcuUnsupportedOperationsFailBeforeTransportIo)
{
    struct Case
    {
        const char *protocol;
        const char *mcu;
        FlashOperation operation;
        std::size_t image_size;
    };
    constexpr std::array cases{
        Case{"sub_tcu_denso_sh7055_can", "SH7055", FlashOperation::Write, 0x80000},
        Case{"sub_tcu_denso_sh7055_can", "SH7055", FlashOperation::TestWrite, 0x80000},
        Case{"sub_tcu_denso_sh7058_can", "SH7058", FlashOperation::TestWrite, 0x100000},
    };

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectNoBackendIo(*fake);

    for (const Case& test : cases)
    {
        auto input = request(test.protocol, test.operation);
        input.mcu = test.mcu;
        input.paths = *paths;
        input.image = bytes::Bytes(test.image_size, 0xa5);
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << test.protocol;

        const auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
    }
}

TEST(FlashWorkflowTest, tcuReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectCanTransportSetup(*fake, false, 2017, 2025);

    auto input = request("sub_tcu_denso_sh7055_can");
    input.mcu = "SH7055";
    input.paths = *paths;
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    ASSERT_TRUE(QFile::remove(QString::fromStdString(paths->protocols_file)));
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_tcu_sh7055.bin")));

    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->plan();
    ASSERT_EQ(plan.transport(), TransportKind::CanIso15765);
    ASSERT_TRUE(plan.kernel().has_value());
    ASSERT_EQ(plan.kernel()->bytes, bytes::Bytes({0x10, 0x20, 0x30, 0x40}));

    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(2);
    NullEventSink events;
    const auto result = attempt.attempt->run(*attempt.clock, cancellation, events);
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::Cancelled);
}

TEST(FlashWorkflowTest, tcuSuccessfulReadPropagatesBytesAndRomId)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto input = request("sub_tcu_denso_sh7058_can");
    input.mcu = "SH7058";
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a, 0xa5}, .rom_id = "123456789A_"});
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5a, 0xa5}));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
}

TEST(FlashWorkflowTest, mc68ResolvesKernelThroughCatalogBeforePromptAndAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02");
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        FAIL() << failure->error.detail.c_str();
    }
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);

    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    ASSERT_TRUE(plan.kernel().has_value());
    ASSERT_EQ(plan.kernel()->load_address, 0x20000U);
    ASSERT_EQ(plan.kernel()->bytes, bytes::Bytes({0x11, 0x22, 0x33}));
}

TEST(FlashWorkflowTest, missingCatalogKernelFailsBeforePrompt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02");
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory, false);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    const auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

TEST(FlashWorkflowTest, sh7055IteratesConfirmationsAndPropagatesAttemptResult)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_sh7055_02");
    input.mcu = "SH7055";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::CycleIgnition);
    workflow->submit(FlashPromptResponse::Accept);

    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    ASSERT_TRUE(plan.kernel().has_value());
    ASSERT_EQ(plan.kernel()->load_address, 0xFFFF6004U);
    ASSERT_EQ(plan.kernel()->bytes, bytes::Bytes({0xaa, 0xbb, 0xcc, 0xdd}));

    workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
    const auto& done = std::get<FlashCompletedStep>(step);
    ASSERT_EQ(done.outcome, FlashWorkflowOutcome::Succeeded);
    ASSERT_EQ(done.accepted_read_bytes, bytes::Bytes({0x5a}));
    ASSERT_EQ(done.rom_id, std::string("123456789A_"));
}

TEST(FlashWorkflowTest, sh7055EcutekResolvesWithoutCarModelReference)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_sh7055_02_ecutek");
    input.mcu = "SH7055";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    ASSERT_EQ(plan.target_id(), std::string_view("sub_ecu_denso_sh7055_02_ecutek"));
    ASSERT_TRUE(plan.kernel().has_value());
    ASSERT_EQ(plan.kernel()->load_address, 0xFFFF6004U);
}

TEST(FlashWorkflowTest, portableImageCopiesRomForEveryNonReadOperation)
{
    const bytes::Bytes rom{0x11, 0x22};
    ASSERT_TRUE(!portableImageForOperation(FlashOperation::Read, rom).has_value());
    ASSERT_EQ(portableImageForOperation(FlashOperation::Write, rom), rom);
    ASSERT_EQ(portableImageForOperation(FlashOperation::TestWrite, rom), rom);
}

TEST(FlashWorkflowTest, mc68TestWriteWithPortableImageReachesAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02", FlashOperation::TestWrite);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    const bytes::Bytes packed_image(0x28000, 0x5a);
    input.image = portableImageForOperation(input.operation, packed_image);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    ASSERT_EQ(std::get<FlashAttempt>(step).attempt->plan().image(), packed_image);
}

TEST(FlashWorkflowTest, mc68PhysicalImageIsPackedAtWorkflowBoundary)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;

    bytes::Bytes physical_image(0x30000, 0xee);
    std::fill_n(physical_image.begin(), 0x20000, 0x11);
    std::fill(physical_image.begin() + 0x28000, physical_image.end(), 0x22);
    input.image = physical_image;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        FAIL() << failure->error.detail.c_str();
    }
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& packed = std::get<FlashAttempt>(step).attempt->plan().image();
    ASSERT_TRUE(packed.has_value());
    ASSERT_EQ(packed->size(), std::size_t{0x28000});
    ASSERT_TRUE(
        std::all_of(packed->begin(), packed->begin() + 0x20000, [](bytes::Byte value) { return value == 0x11; }));
    ASSERT_TRUE(std::all_of(packed->begin() + 0x20000, packed->end(), [](bytes::Byte value) { return value == 0x22; }));
}

TEST(FlashWorkflowTest, mc68CalibrationPaddingRoundTripsToPackedWriteImage)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02", FlashOperation::TestWrite);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;

    bytes::Bytes packed_image(0x28000);
    for (std::size_t index = 0; index < packed_image.size(); ++index)
    {
        packed_image[index] = static_cast<bytes::Byte>((index / 0x4000) + 1);
    }
    input.image = calibration::apply_flash_method_padding(packed_image, "sub_ecu_denso_mc68hc16y5_02");
    ASSERT_EQ(input.image->size(), std::size_t{0x30000});

    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    ASSERT_EQ(std::get<FlashAttempt>(step).attempt->plan().image(), packed_image);
}

TEST(FlashWorkflowTest, sh7055TestWriteWithPortableImageReachesPromptsAndAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_sh7055_02", FlashOperation::TestWrite);
    input.mcu = "SH7055";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    input.image = portableImageForOperation(input.operation, bytes::Bytes(0x80000, 0xa5));
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::CycleIgnition);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
}

TEST(FlashWorkflowTest, mc68TpuReadResolvesCatalogAndReachesAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02_tpu");
    input.mcu = "MC68HC16Y5_TPU";
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& kernel = std::get<FlashAttempt>(step).attempt->plan().kernel();
    ASSERT_TRUE(kernel.has_value());
    ASSERT_EQ(kernel->load_address, 0x20000U);
    ASSERT_EQ(kernel->bytes, bytes::Bytes({0x44, 0x55, 0x66}));
}

TEST(FlashWorkflowTest, densoSh705xKlineRoutesExactProtocolsThroughBeginToAttempt)
{
    struct Case
    {
        const char *protocol;
        const char *mcu;
        FlashOperation operation;
        SubaruDensoSh705xKlineSeedKey seed_key;
        bytes::Bytes kernel;
    };
    const std::vector<Case> cases{
        {"sub_ecu_denso_sh7055_04",
         "SH7055",
         FlashOperation::Read,
         SubaruDensoSh705xKlineSeedKey::Stock,
         {0xaa, 0xbb, 0xcc, 0xdd}},
        {"sub_ecu_denso_sh7058_ecutek",
         "SH7058",
         FlashOperation::Read,
         SubaruDensoSh705xKlineSeedKey::EcuTek,
         {0x01, 0x02, 0x03, 0x04}},
        {"sub_ecu_denso_sh7058_cobb",
         "SH7058",
         FlashOperation::TestWrite,
         SubaruDensoSh705xKlineSeedKey::Stock,
         {0x01, 0x02, 0x03, 0x04}},
    };
    for (const Case& c : cases)
    {
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const auto paths = catalogPaths(directory);
        ASSERT_TRUE(paths.has_value());
        auto input = request(c.protocol, c.operation);
        input.mcu = c.mcu;
        input.paths = *paths;
        if (c.operation != FlashOperation::Read)
        {
            input.image = bytes::Bytes(std::size_t{1024} * 1024, 0xff);
        }
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << c.protocol;

        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << c.protocol;
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        ASSERT_EQ(plan.family(), FlashFamily::SubaruDensoSh705xKline);
        ASSERT_EQ(plan.target_id(), std::string(c.protocol));
        ASSERT_TRUE(plan.kernel().has_value());
        ASSERT_EQ(plan.kernel()->bytes, c.kernel);
        ASSERT_EQ(std::get<SubaruDensoSh705xKlinePlan>(plan.family_plan()).seed_key, c.seed_key);
    }
}

TEST(FlashWorkflowTest, densoSh705xKlineCobbReadFailsBeforeAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto input = request("sub_ecu_denso_sh7058_cobb");
    input.mcu = "SH7058";
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

TEST(FlashWorkflowTest, densoSh705xKlineIgnoresPrefixLookalikes)
{
    for (const char *near_miss :
         {"sub_ecu_denso_sh7055_04_future", "sub_ecu_denso_sh7058_extra", "sub_ecu_denso_sh7058_ecutek_racerom"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr) << near_miss;
    }
}

TEST(FlashWorkflowTest, unisiaJecsM32rRoutesTheFourExactProtocols)
{
    struct Variant
    {
        const char *protocol;
        const char *mcu;
        std::uint32_t rom_size;
    };
    for (const Variant& variant : std::to_array<Variant>({
             {"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000},
             {"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000},
             {"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000},
             {"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000},
         }))
    {
        auto input = request(variant.protocol);
        input.mcu = variant.mcu;
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << variant.protocol;
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << variant.protocol;
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        ASSERT_EQ(plan.family(), FlashFamily::SubaruUnisiaJecsM32rKline);
        ASSERT_EQ(plan.transfer_region(), (MemoryRegion{0x100000, variant.rom_size}));
    }
}

TEST(FlashWorkflowTest, unisiaJecsM32rLookalikesStayUnrouted)
{
    for (const char *protocol : {"sub_ecu_unisia_jecs_20x", "sub_ecu_unisia_jecs_7", "sub_ecu_unisia_jecs_20_bootmodex",
                                 "sub_ecu_unisia_jecs_40_bootmode"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(request(protocol)) == nullptr) << protocol;
    }
}

TEST(FlashWorkflowTest, unisiaBootmodeReadUsesTheKlineReadFamily)
{
    struct Variant
    {
        const char *protocol;
        const char *mcu;
        std::uint32_t rom_size;
    };
    for (const Variant& variant : std::to_array<Variant>({
             {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000},
             {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000},
         }))
    {
        auto input = request(variant.protocol);
        input.mcu = variant.mcu;
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << variant.protocol;
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << variant.protocol;
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        ASSERT_EQ(plan.family(), FlashFamily::SubaruUnisiaJecsM32rKline);
        ASSERT_EQ(plan.transfer_region(), (MemoryRegion{0x100000, variant.rom_size}));
        ASSERT_TRUE(plan.confirmations().empty());
        workflow->submit(
            FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1}, .rom_id = std::string("123456789A_")});
        const auto done = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
        ASSERT_TRUE(std::get<FlashCompletedStep>(done).rom_id == std::optional<std::string>("123456789A_"));
    }
}

TEST(FlashWorkflowTest, unisiaBootmodeWriteRunsKernelThenMod1ThenProgram)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(*paths));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        FAIL() << failure->error.detail.c_str();
    }
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ApplyBootModeVoltages);
    workflow->submit(FlashPromptResponse::Accept);

    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& kernel = std::get<FlashAttempt>(step).attempt->plan();
    ASSERT_EQ(kernel.family(), FlashFamily::SubaruUnisiaJecsM32rBootModeKernel);
    bytes::Bytes padded{0x01, 0x02, 0x03, 0x04, 0x05};
    padded.resize(0x80, 0x00);
    ASSERT_EQ(kernel.image(), std::optional<bytes::Bytes>(padded));
    workflow->submit(FlashAttemptResult{.success = true});

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::RemoveMod1);
    workflow->submit(FlashPromptResponse::Accept);

    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& program = std::get<FlashAttempt>(step).attempt->plan();
    ASSERT_EQ(program.family(), FlashFamily::SubaruUnisiaJecsM32rBootModeProgram);
    ASSERT_EQ(program.image(), std::optional<bytes::Bytes>(bytes::Bytes(0x20000, 0xa5)));
    workflow->submit(FlashAttemptResult{.success = true});

    const auto notice = std::get<FlashPromptStep>(workflow->next());
    ASSERT_EQ(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == bootmodeNotice("succeeded"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
}

TEST(FlashWorkflowTest, unisiaBootmodeKernelFailureSkipsMod1AndProgram)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = unisiaBootmodeAtKernelAttempt(*paths);
    ASSERT_TRUE(workflow != nullptr);
    workflow->submit(
        FlashAttemptResult{.success = false, .error_kind = ErrorKind::InvalidConfig, .error_detail = "baud"});
    const auto notice = std::get<FlashPromptStep>(workflow->next());
    ASSERT_EQ(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == bootmodeNotice("failed"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto failure = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::InvalidConfig);
}

TEST(FlashWorkflowTest, unisiaBootmodeKernelCancelledShowsNotice)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = unisiaBootmodeAtKernelAttempt(*paths);
    ASSERT_TRUE(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::Cancelled});
    const auto notice = std::get<FlashPromptStep>(workflow->next());
    ASSERT_EQ(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == bootmodeNotice("cancelled"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

TEST(FlashWorkflowTest, unisiaBootmodeDeclinedMod1CancelsWithNotice)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = unisiaBootmodeAtKernelAttempt(*paths);
    ASSERT_TRUE(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = true});
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::RemoveMod1);
    workflow->submit(FlashPromptResponse::Decline);
    const auto notice = std::get<FlashPromptStep>(workflow->next());
    ASSERT_EQ(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == bootmodeNotice("cancelled"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

TEST(FlashWorkflowTest, unisiaBootmodeProgramFailureShowsNoticeThenFailure)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = unisiaBootmodeAtKernelAttempt(*paths);
    ASSERT_TRUE(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = true});
    workflow->next(); // RemoveMod1
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::BadResponse, .error_detail = "x"});
    const auto notice = std::get<FlashPromptStep>(workflow->next());
    ASSERT_TRUE(notice.arguments == bootmodeNotice("failed"));
    workflow->submit(FlashPromptResponse::Accept);
    const auto failure = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::BadResponse);
}

TEST(FlashWorkflowTest, unisiaBootmodeDeclinedVoltagesCancelsBeforeAnyAttempt)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(*paths));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ApplyBootModeVoltages);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

TEST(FlashWorkflowTest, unisiaBootmodeWrongImageSizeFailsBeforeAnyPrompt)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto input = unisiaBootmodeWrite(*paths);
    input.image = bytes::Bytes(0x20001, 0xa5);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    const auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

TEST(FlashWorkflowTest, unisiaBootmodeMissingKernelFailsBeforeAnyPrompt)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory, false);
    ASSERT_TRUE(paths.has_value());
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(*paths));
    const auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_TRUE(std::get<FlashFailureStep>(step).error.detail.find("catalog_uj20_bootmode.bin") != std::string::npos);
}

TEST(FlashWorkflowTest, unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    ASSERT_TRUE(writeFile(directory.filePath("kernels/catalog_uj20_bootmode.bin"), QByteArray()));
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaBootmodeWrite(*paths));
    const auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

TEST(FlashWorkflowTest, unisiaBootmodeTestWriteIsUnsupported)
{
    QTemporaryDir directory;
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto input = unisiaBootmodeWrite(*paths);
    input.operation = FlashOperation::TestWrite;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    const auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

TEST(FlashWorkflowTest, unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter)
{
    // request() carries a null serial: no adapter information means prompting.
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaM32rWrite());
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ApplyProgrammingVoltage);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    ASSERT_EQ(plan.confirmations().size(), std::size_t{1});
    ASSERT_EQ(plan.confirmations()[0].id, ConfirmationSpec::Id::ApplyProgrammingVoltage);

    workflow->submit(FlashAttemptResult{.success = true});
    const auto reminder = std::get<FlashPromptStep>(workflow->next());
    ASSERT_EQ(reminder.kind, FlashPromptKind::RemoveProgrammingVoltage);
    ASSERT_TRUE(reminder.arguments == (PromptArguments{{"outcome", "succeeded"}, {"external_vpp", "yes"}}));
    workflow->submit(FlashPromptResponse::Accept);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
}

TEST(FlashWorkflowTest, unisiaJecsM32rFailedWriteRemindsBeforeReportingTheFailure)
{
    auto workflow = unisiaM32rWriteAtAttempt();
    ASSERT_TRUE(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::BadResponse, .error_detail = "x"});
    const auto reminder = std::get<FlashPromptStep>(workflow->next());
    ASSERT_EQ(reminder.kind, FlashPromptKind::RemoveProgrammingVoltage);
    ASSERT_TRUE(reminder.arguments == (PromptArguments{{"outcome", "failed"}, {"external_vpp", "yes"}}));
    workflow->submit(FlashPromptResponse::Accept);
    const auto failure = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::BadResponse);
}

TEST(FlashWorkflowTest, unisiaJecsM32rCancelledWriteReminds)
{
    auto workflow = unisiaM32rWriteAtAttempt();
    ASSERT_TRUE(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::Cancelled});
    const auto reminder = std::get<FlashPromptStep>(workflow->next());
    ASSERT_EQ(reminder.kind, FlashPromptKind::RemoveProgrammingVoltage);
    ASSERT_TRUE(reminder.arguments == (PromptArguments{{"outcome", "cancelled"}, {"external_vpp", "yes"}}));
    workflow->submit(FlashPromptResponse::Decline); // OK-only notice; any answer continues
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

TEST(FlashWorkflowTest, unisiaJecsM32rDeclinedVppPromptCancelsBeforeAttempt)
{
    auto workflow = FlashWorkflowFactory::tryCreate(unisiaM32rWrite());
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ApplyProgrammingVoltage);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

TEST(FlashWorkflowTest, unisiaJecsM32rAdapterSuppliedVppSkipsBothPrompts)
{
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ASSERT_TRUE(serial->set_use_openport2_adapter(true));
    auto input = unisiaM32rWrite();
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    ASSERT_TRUE(std::get<FlashAttempt>(step).attempt->plan().confirmations().empty());
    workflow->submit(FlashAttemptResult{.success = true});
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
}

// OpenPort 2.0 supplies VPP, so no remove-VPP sentence is due; legacy still
// warned on every failed write not to power off the ECU.
std::unique_ptr<FlashWorkflow> unisiaM32rOpenPort2WriteAtAttempt(std::unique_ptr<SerialPortActions>& serial,
                                                                 FakeBackend **fake)
{
    serial = recordingSerial(fake);
    if (serial == nullptr || !serial->set_use_openport2_adapter(true))
    {
        return nullptr;
    }
    auto input = unisiaM32rWrite();
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    if (workflow == nullptr || std::get<FlashPromptStep>(workflow->next()).kind != FlashPromptKind::Begin)
    {
        return nullptr;
    }
    workflow->submit(FlashPromptResponse::Accept);
    if (!std::holds_alternative<FlashAttempt>(workflow->next()))
    {
        return nullptr;
    }
    return workflow;
}

TEST(FlashWorkflowTest, unisiaJecsM32rAdapterSuppliedVppFailedWriteWarnsNotToPowerOff)
{
    FakeBackend *fake = nullptr;
    std::unique_ptr<SerialPortActions> serial;
    auto workflow = unisiaM32rOpenPort2WriteAtAttempt(serial, &fake);
    ASSERT_TRUE(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::Timeout, .error_detail = "x"});
    const auto notice_step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(notice_step));
    const auto& notice = std::get<FlashPromptStep>(notice_step);
    ASSERT_EQ(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == (PromptArguments{{"outcome", "failed"}, {"external_vpp", "no"}}));
    workflow->submit(FlashPromptResponse::Accept);
    const auto failure = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::Timeout);
}

TEST(FlashWorkflowTest, unisiaJecsM32rAdapterSuppliedVppCancelledWriteWarnsNotToPowerOff)
{
    FakeBackend *fake = nullptr;
    std::unique_ptr<SerialPortActions> serial;
    auto workflow = unisiaM32rOpenPort2WriteAtAttempt(serial, &fake);
    ASSERT_TRUE(workflow != nullptr);
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::Cancelled});
    const auto notice_step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(notice_step));
    const auto& notice = std::get<FlashPromptStep>(notice_step);
    ASSERT_EQ(notice.kind, FlashPromptKind::RemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == (PromptArguments{{"outcome", "cancelled"}, {"external_vpp", "no"}}));
    workflow->submit(FlashPromptResponse::Accept);
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

TEST(FlashWorkflowTest, unisiaJecsM32rReadPropagatesRomIdWithoutVppPrompts)
{
    auto input = request("sub_ecu_unisia_jecs_30");
    input.mcu = "M32R_256KB";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1, 2}, .rom_id = std::string("123456789A_")});
    const auto done = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_TRUE(std::get<FlashCompletedStep>(done).rom_id == std::optional<std::string>("123456789A_"));
    ASSERT_TRUE(std::get<FlashCompletedStep>(done).accepted_read_bytes ==
                std::optional<bytes::Bytes>(bytes::Bytes{1, 2}));
}

TEST(FlashWorkflowTest, unisiaJecsM32rFailedReadReportsWithoutNotice)
{
    auto input = request("sub_ecu_unisia_jecs_30");
    input.mcu = "M32R_256KB";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::Timeout, .error_detail = "x"});
    const auto failure = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::Timeout);
}

TEST(FlashWorkflowTest, unisiaJecsM32rWriteOnReadOnlyVariantFailsBeforeAnyPrompt)
{
    auto input = request("sub_ecu_unisia_jecs_40", FlashOperation::Write);
    input.mcu = "M32R_384KB";
    input.image = bytes::Bytes(0x60000, 0xff);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    const auto step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

// Characterization of the twenty-four single-attempt families' Read path:
// ten kernel-free CAN families, six kernel-free K-Line families, Colt, and
// seven kernel-backed families. Each runs preflight, Begin, the plan's
// confirmations in order, then exactly one attempt. Hitachi SH7058 and Denso
// MC68HC16Y5 BDM read over K-Line; their Write sides are characterized after
// this suite.
struct SingleAttemptCase
{
    std::string_view protocol;
    std::string_view mcu;
    FlashFamily family;
    TransportKind transport;
    // Kernel-backed families only: the catalog kernel the plan must carry.
    std::optional<KernelImage> kernel;
    // Read prompts after Begin.
    std::vector<FlashPromptKind> confirmations;
};

KernelImage catalogKernel(std::string_view protocol, std::uint32_t load_address, bytes::Bytes bytes)
{
    return KernelImage{
        .id = std::format("{}-kernel", protocol), .load_address = load_address, .bytes = std::move(bytes)};
}

std::vector<SingleAttemptCase> singleAttemptCases()
{
    using enum FlashFamily;
    using enum TransportKind;
    return {
        {"sub_ecu_hitachi_m32r_can", "M32R_512KB_1block", SubaruHitachiM32rCan, CanIso15765, std::nullopt, {}},
        {"sub_tcu_cvt_hitachi_m32r_can", "M32R_512KB", SubaruTcuCvtHitachiM32rCan, CanIso15765, std::nullopt, {}},
        {"sub_tcu_cvt_mitsu_mh8111_can", "MH8111", SubaruTcuCvtMitsuMh8111Can, CanIso15765, std::nullopt, {}},
        {"sub_tcu_cvt_mitsu_mh8104_can", "MH8104", SubaruTcuCvtMitsuMh8104Can, CanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_1n83m_1_5m_can", "N83M_1_5MB", SubaruDenso1n83m_1_5mCan, CanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_sh72531_can", "SH72531", SubaruDensoSh72531Can, CanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_sh72543_can_diesel", "SH72543d", SubaruDensoSh72543CanDiesel, CanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_1n83m_4m_can", "N83M_4MB", SubaruDenso1n83m_4mCan, CanIso15765, std::nullopt, {}},
        {"sub_tcu_hitachi_m32r_can", "M32R_512KB", SubaruTcuHitachiM32rCan, CanIso15765, std::nullopt, {}},
        {"sub_ecu_hitachi_sh72543r_can", "SH72543R", SubaruHitachiSh72543rCan, CanIso15765, std::nullopt, {}},
        {"sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks", SubaruMitsuM32rKline, Kline, std::nullopt, {}},
        {"sub_ecu_hitachi_m32r_kline", "M32R_512KB_1block", SubaruHitachiM32rKline, Kline, std::nullopt, {}},
        {"sub_tcu_hitachi_m32r_kline", "M32R_512KB", SubaruTcuHitachiM32rKline, Kline, std::nullopt, {}},
        {"sub_ecu_unisia_jecs_m3779x", "M3779x", SubaruUnisiaJecs, Kline, std::nullopt, {}},
        {"sub_ecu_hitachi_sh7058_can",
         "SH7058_1block",
         SubaruHitachiSh7058,
         Kline,
         std::nullopt,
         {FlashPromptKind::ConfirmSh7058Read}},
        {"sub_ecu_denso_mc68hc16y5_02_bdm", "MC68HC16Y5", SubaruDensoMc68hc16y5_02Bdm, Kline, std::nullopt, {}},
        {"mitsu_ecu_m32r_can", "M32R_384KB_1block", MitsuColtM32rCan, CanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_sh7055_densocan",
         "SH7055",
         SubaruDensoSh705xDensoCan,
         CanRawIso15765,
         catalogKernel("sub_ecu_denso_sh7055_densocan", 0xFFFF6004, {0xaa, 0xbb, 0xcc, 0xdd}),
         {FlashPromptKind::CycleIgnition}},
        {"sub_tcu_denso_sh7055_can",
         "SH7055",
         SubaruTcuDensoSh705xCan,
         CanIso15765,
         catalogKernel("sub_tcu_denso_sh7055_can", 0xFFFF9000, {0x10, 0x20, 0x30, 0x40}),
         {}},
        {"sub_ecu_denso_sh7058_can",
         "SH7058",
         SubaruDensoSh7058Can,
         CanIso15765,
         catalogKernel("sub_ecu_denso_sh7058_can", 0xFFFF3000, {0x90, 0xa0, 0xb0, 0xc0}),
         {}},
        {"sub_ecu_denso_sh7058_can_diesel",
         "SH7058d",
         SubaruDensoSh7058CanDiesel,
         CanIso15765,
         catalogKernel("sub_ecu_denso_sh7058_can_diesel", 0xFFFF4000, {0xd0, 0xe0, 0xf0, 0x01}),
         {}},
        {"sub_ecu_denso_sh7055_04",
         "SH7055",
         SubaruDensoSh705xKline,
         Kline,
         catalogKernel("sub_ecu_denso_sh7055_04", 0xFFFF6004, {0xaa, 0xbb, 0xcc, 0xdd}),
         {}},
        {"sub_ecu_denso_sh7055_02",
         "SH7055",
         SubaruDensoSh7055_02,
         Kline,
         catalogKernel("sub_ecu_denso_sh7055_02", 0xFFFF6004, {0xaa, 0xbb, 0xcc, 0xdd}),
         {FlashPromptKind::CycleIgnition}},
        {"sub_ecu_denso_mc68hc16y5_02",
         "MC68HC16Y5",
         SubaruDensoMc68hc16y5_02,
         Kline,
         catalogKernel("sub_ecu_denso_mc68hc16y5_02", 0x20000, {0x11, 0x22, 0x33}),
         {}},
    };
}

FlashWorkflowRequest singleAttemptRead(const SingleAttemptCase& test, const config::ConfigPaths& paths)
{
    auto input = request(std::string(test.protocol));
    input.mcu = std::string(test.mcu);
    input.paths = paths;
    return input;
}

std::vector<FlashPromptKind> promptKinds(const std::vector<FlashPromptStep>& prompts)
{
    std::vector<FlashPromptKind> kinds;
    std::ranges::transform(prompts, std::back_inserter(kinds), &FlashPromptStep::kind);
    return kinds;
}

std::vector<FlashPromptKind> withBegin(const std::vector<FlashPromptKind>& confirmations)
{
    std::vector<FlashPromptKind> kinds{FlashPromptKind::Begin};
    kinds.insert(kinds.end(), confirmations.begin(), confirmations.end());
    return kinds;
}

// Accepts every prompt, recording it, and returns the first step that is not
// a prompt. The bound keeps a workflow that never stops prompting from
// hanging the suite.
FlashWorkflowStep acceptEveryPrompt(FlashWorkflow& workflow, std::vector<FlashPromptStep>& prompts)
{
    for (int bound = 0; bound < 8; ++bound)
    {
        FlashWorkflowStep step = workflow.next();
        const auto *prompt = std::get_if<FlashPromptStep>(&step);
        if (prompt == nullptr)
        {
            return step;
        }
        prompts.push_back(*prompt);
        workflow.submit(FlashPromptResponse::Accept);
    }
    return FlashFailureStep{Error{ErrorKind::Internal, "workflow kept prompting"}};
}

std::string failureDetail(const FlashWorkflowStep& step)
{
    const auto *failure = std::get_if<FlashFailureStep>(&step);
    return failure == nullptr ? std::string() : failure->error.detail;
}

TEST(FlashWorkflowTest, singleAttemptFamiliesPromptInOrderAndBindTheirExecutorOnce)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const SingleAttemptCase& test : singleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);
        auto workflow = FlashWorkflowFactory::tryCreate(singleAttemptRead(test, *paths));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = acceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << failureDetail(step);
        EXPECT_THAT(promptKinds(prompts), ::testing::ElementsAreArray(withBegin(test.confirmations)));
        EXPECT_THAT(prompts, ::testing::Each(::testing::Field(&FlashPromptStep::arguments, ::testing::IsEmpty())));

        auto& attempt = std::get<FlashAttempt>(step);
        ASSERT_TRUE(attempt.attempt != nullptr);
        ASSERT_TRUE(attempt.clock != nullptr);
        const FlashPlan& plan = attempt.attempt->plan();
        EXPECT_EQ(plan.family(), test.family);
        EXPECT_EQ(plan.transport(), test.transport);
        EXPECT_EQ(plan.target_id(), test.protocol);
        EXPECT_EQ(plan.mcu_name(), test.mcu);
        EXPECT_EQ(plan.operation(), FlashOperation::Read);
        EXPECT_EQ(plan.kernel().has_value(), test.kernel.has_value());

        // transport_setup() rejects a plan from another family, so reaching
        // the pre-configure cancellation proves the bound executor owns this
        // plan. Nothing is configured or opened on the way.
        FakeCancellationToken cancelled(true);
        NullEventSink events;
        EXPECT_THAT(attempt.attempt->run(*attempt.clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::Cancelled, ::testing::HasSubstr("before configure")));

        // Exactly one attempt: neither the pending nor the finished workflow
        // hands out a second one.
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->next()));
        workflow->submit(FlashAttemptResult{.success = true});
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->next()));
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->next()));
    }
}

TEST(FlashWorkflowTest, singleAttemptFamiliesCancelWhenAnyPromptIsDeclined)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const SingleAttemptCase& test : singleAttemptCases())
    {
        const std::vector<FlashPromptKind> sequence = withBegin(test.confirmations);
        for (std::size_t declined = 0; declined < sequence.size(); ++declined)
        {
            SCOPED_TRACE(std::format("{} declining prompt {}", test.protocol, declined));
            auto workflow = FlashWorkflowFactory::tryCreate(singleAttemptRead(test, *paths));
            ASSERT_TRUE(workflow != nullptr);
            for (std::size_t accepted = 0; accepted < declined; ++accepted)
            {
                ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, sequence[accepted]);
                workflow->submit(FlashPromptResponse::Accept);
            }
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, sequence[declined]);
            workflow->submit(FlashPromptResponse::Decline);

            for (int repeat = 0; repeat < 2; ++repeat)
            {
                const auto step = workflow->next();
                ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
                const auto& done = std::get<FlashCompletedStep>(step);
                EXPECT_EQ(done.outcome, FlashWorkflowOutcome::Cancelled);
                EXPECT_FALSE(done.accepted_read_bytes.has_value());
                EXPECT_FALSE(done.rom_id.has_value());
            }
        }
    }
}

TEST(FlashWorkflowTest, singleAttemptFamiliesTreatEveryNonAcceptResponseAsDecline)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const SingleAttemptCase& test : singleAttemptCases())
    {
        for (const auto response : {FlashPromptResponse::Save, FlashPromptResponse::Discard})
        {
            SCOPED_TRACE(std::format("{} response {}", test.protocol, static_cast<int>(response)));
            auto workflow = FlashWorkflowFactory::tryCreate(singleAttemptRead(test, *paths));
            ASSERT_TRUE(workflow != nullptr);
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
            workflow->submit(response);
            const auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Cancelled);
        }
    }
}

std::unique_ptr<FlashWorkflow> singleAttemptAtAttempt(const SingleAttemptCase& test, const config::ConfigPaths& paths)
{
    auto workflow = FlashWorkflowFactory::tryCreate(singleAttemptRead(test, paths));
    std::vector<FlashPromptStep> prompts;
    if (workflow == nullptr || !std::holds_alternative<FlashAttempt>(acceptEveryPrompt(*workflow, prompts)))
    {
        return nullptr;
    }
    return workflow;
}

TEST(FlashWorkflowTest, singleAttemptFamiliesReportEveryAttemptOutcome)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const SingleAttemptCase& test : singleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);

        auto with_identity = singleAttemptAtAttempt(test, *paths);
        ASSERT_TRUE(with_identity != nullptr);
        with_identity->submit(
            FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a, 0xa5}, .rom_id = "CAL_123456789A_"});
        auto step = with_identity->next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Succeeded);
        EXPECT_EQ(std::get<FlashCompletedStep>(step).accepted_read_bytes, bytes::Bytes({0x5a, 0xa5}));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).rom_id, std::string("CAL_123456789A_"));

        auto without_identity = singleAttemptAtAttempt(test, *paths);
        ASSERT_TRUE(without_identity != nullptr);
        without_identity->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x01}});
        step = without_identity->next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Succeeded);
        EXPECT_EQ(std::get<FlashCompletedStep>(step).accepted_read_bytes, bytes::Bytes({0x01}));
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).rom_id.has_value());

        auto failed = singleAttemptAtAttempt(test, *paths);
        ASSERT_TRUE(failed != nullptr);
        failed->submit(FlashAttemptResult{.success = false,
                                          .error_kind = ErrorKind::Timeout,
                                          .error_detail = "no reply to 0x34",
                                          .read_bytes = bytes::Bytes{0x02},
                                          .rom_id = "IGNORED"});
        step = failed->next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::Timeout, "no reply to 0x34"}));

        auto cancelled = singleAttemptAtAttempt(test, *paths);
        ASSERT_TRUE(cancelled != nullptr);
        cancelled->submit(FlashAttemptResult{.success = false,
                                             .error_kind = ErrorKind::Cancelled,
                                             .error_detail = "cancelled: dialog closed",
                                             .read_bytes = bytes::Bytes{0x03}});
        step = cancelled->next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Cancelled);
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).accepted_read_bytes.has_value());
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).rom_id.has_value());
    }
}

TEST(FlashWorkflowTest, singleAttemptFamiliesRejectInvalidConfigurationBeforeAnyPromptOrIo)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectNoBackendIo(*fake);

    for (const SingleAttemptCase& test : singleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);
        auto input = singleAttemptRead(test, *paths);
        input.mcu = "NOT_A_KNOWN_MCU";
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
        }
    }
}

TEST(FlashWorkflowTest, kernelBackedFamiliesFailBeforeAnyPromptOrIoWithoutTheirKernel)
{
    QTemporaryDir without_kernels;
    ASSERT_TRUE(without_kernels.isValid());
    const auto catalog_only = catalogPaths(without_kernels, false);
    ASSERT_TRUE(catalog_only.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectNoBackendIo(*fake);

    for (const SingleAttemptCase& test : singleAttemptCases())
    {
        if (!test.kernel.has_value())
        {
            continue;
        }
        for (const bool has_catalog : {true, false})
        {
            SCOPED_TRACE(std::format("{} with{} catalog", test.protocol, has_catalog ? "" : "out"));
            auto input = singleAttemptRead(test, has_catalog ? *catalog_only : config::ConfigPaths{});
            input.serial = serial.get();
            auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
            ASSERT_TRUE(workflow != nullptr);
            for (int repeat = 0; repeat < 2; ++repeat)
            {
                const auto step = workflow->next();
                ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
                EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
            }
        }
    }
}

TEST(FlashWorkflowTest, kernelBackedFamiliesKeepTheirKernelSnapshotAfterFilesAreRemoved)
{
    for (const SingleAttemptCase& test : singleAttemptCases())
    {
        if (!test.kernel.has_value())
        {
            continue;
        }
        SCOPED_TRACE(test.protocol);
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const auto paths = catalogPaths(directory);
        ASSERT_TRUE(paths.has_value());
        auto workflow = FlashWorkflowFactory::tryCreate(singleAttemptRead(test, *paths));
        ASSERT_TRUE(workflow != nullptr);

        // The kernel is loaded before Begin; nothing on disk is read afterward.
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        ASSERT_TRUE(QFile::remove(QString::fromStdString(paths->protocols_file)));
        ASSERT_TRUE(QDir(directory.filePath("kernels")).removeRecursively());
        workflow->submit(FlashPromptResponse::Accept);

        std::vector<FlashPromptStep> prompts;
        auto step = acceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << failureDetail(step);
        EXPECT_THAT(promptKinds(prompts), ::testing::ElementsAreArray(test.confirmations));
        const auto& kernel = std::get<FlashAttempt>(step).attempt->plan().kernel();
        ASSERT_TRUE(kernel.has_value());
        EXPECT_EQ(kernel->id, test.kernel->id);
        EXPECT_EQ(kernel->load_address, test.kernel->load_address);
        EXPECT_EQ(kernel->bytes, test.kernel->bytes);
    }
}

// Hitachi SH7058 Write runs over CAN with Begin as its only prompt; its Read
// side (K-Line) is in singleAttemptCases().
FlashWorkflowRequest sh7058Write()
{
    auto input = request("sub_ecu_hitachi_sh7058_can", FlashOperation::Write);
    input.mcu = "SH7058_1block";
    input.image = bytes::Bytes(0x100000, 0x5a);
    return input;
}

TEST(FlashWorkflowTest, sh7058WriteBindsTheCanExecutorAfterBeginAlone)
{
    auto workflow = FlashWorkflowFactory::tryCreate(sh7058Write());
    ASSERT_TRUE(workflow != nullptr);

    std::vector<FlashPromptStep> prompts;
    auto step = acceptEveryPrompt(*workflow, prompts);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << failureDetail(step);
    EXPECT_THAT(promptKinds(prompts), ::testing::ElementsAre(FlashPromptKind::Begin));

    auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->plan();
    EXPECT_EQ(plan.family(), FlashFamily::SubaruHitachiSh7058);
    EXPECT_EQ(plan.transport(), TransportKind::CanIso15765);
    EXPECT_EQ(plan.operation(), FlashOperation::Write);
    EXPECT_EQ(plan.image(), std::optional<bytes::Bytes>(bytes::Bytes(0x100000, 0x5a)));
    EXPECT_TRUE(plan.confirmations().empty());

    // The K-Line executor's transport_setup() rejects a Write plan as
    // Unsupported, so reaching the pre-configure cancellation proves the CAN
    // executor is bound. Nothing is configured or opened on the way.
    FakeCancellationToken cancelled(true);
    NullEventSink events;
    EXPECT_THAT(attempt.attempt->run(*attempt.clock, cancelled, events),
                fastecu::testing::IsErrWith(ErrorKind::Cancelled, ::testing::HasSubstr("before configure")));

    workflow->submit(
        FlashAttemptResult{.success = false, .error_kind = ErrorKind::Timeout, .error_detail = "no reply to 0x34"});
    step = workflow->next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::Timeout, "no reply to 0x34"}));
}

TEST(FlashWorkflowTest, sh7058WriteCancelsWhenBeginIsDeclined)
{
    auto workflow = FlashWorkflowFactory::tryCreate(sh7058Write());
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Decline);
    for (int repeat = 0; repeat < 2; ++repeat)
    {
        const auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Cancelled);
    }
}

TEST(FlashWorkflowTest, sh7058WriteRejectsInvalidInputBeforeAnyPromptOrIo)
{
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectNoBackendIo(*fake);

    struct Case
    {
        const char *name;
        FlashOperation operation;
        std::size_t image_size;
        ErrorKind expected;
    };
    for (const Case& test : std::to_array<Case>({
             {"test write", FlashOperation::TestWrite, 0x100000, ErrorKind::Unsupported},
             {"short image", FlashOperation::Write, 0xFFFFF, ErrorKind::InvalidConfig},
         }))
    {
        SCOPED_TRACE(test.name);
        auto input = sh7058Write();
        input.operation = test.operation;
        input.image = bytes::Bytes(test.image_size, 0x5a);
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, test.expected);
        }
    }
}

// Denso MC68HC16Y5 BDM Write uploads the catalog kernel to RAM and starts it.
// The desktop hands every Write the operator's ROM; BDM must drop it.
FlashWorkflowRequest bdmWrite(const config::ConfigPaths& paths)
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    input.paths = paths;
    input.image = bytes::Bytes(0x30000, 0x5a);
    return input;
}

// catalog_mc68.bin (11 22 33) zero-padded to the 0x20-byte upload chunk.
bytes::Bytes bdmCatalogKernelImage()
{
    bytes::Bytes image(0x20, 0x00);
    image[0] = 0x11;
    image[1] = 0x22;
    image[2] = 0x33;
    return image;
}

TEST(FlashWorkflowTest, mc68BdmWriteBindsItsExecutorAndReportsEveryOutcome)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const bool succeeded : {true, false})
    {
        SCOPED_TRACE(succeeded ? "succeeded" : "failed");
        auto workflow = FlashWorkflowFactory::tryCreate(bdmWrite(*paths));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = acceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << failureDetail(step);
        EXPECT_THAT(promptKinds(prompts),
                    ::testing::ElementsAre(FlashPromptKind::Begin, FlashPromptKind::ConfirmBdmKernelBootstrap));
        EXPECT_THAT(prompts, ::testing::Each(::testing::Field(&FlashPromptStep::arguments, ::testing::IsEmpty())));

        auto& attempt = std::get<FlashAttempt>(step);
        const FlashPlan& plan = attempt.attempt->plan();
        EXPECT_EQ(plan.family(), FlashFamily::SubaruDensoMc68hc16y5_02Bdm);
        EXPECT_EQ(plan.transport(), TransportKind::Kline);
        EXPECT_EQ(plan.operation(), FlashOperation::Write);
        EXPECT_EQ(plan.image(), std::optional<bytes::Bytes>(bdmCatalogKernelImage()));
        EXPECT_EQ(plan.transfer_region(), (MemoryRegion{0x20000, 0x20}));
        EXPECT_FALSE(plan.kernel().has_value());

        // transport_setup() validates the plan, so reaching the pre-configure
        // cancellation proves the BDM executor owns it.
        FakeCancellationToken cancelled(true);
        NullEventSink events;
        EXPECT_THAT(attempt.attempt->run(*attempt.clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::Cancelled, ::testing::HasSubstr("before configure")));
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->next()));

        if (succeeded)
        {
            workflow->submit(FlashAttemptResult{.success = true});
            step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Succeeded);
            EXPECT_FALSE(std::get<FlashCompletedStep>(step).accepted_read_bytes.has_value());
        }
        else
        {
            workflow->submit(FlashAttemptResult{
                .success = false, .error_kind = ErrorKind::Disconnected, .error_detail = "adapter removed"});
            step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::Disconnected, "adapter removed"}));
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteCancelsWhenEitherPromptIsDeclined)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    const std::vector<FlashPromptKind> sequence{FlashPromptKind::Begin, FlashPromptKind::ConfirmBdmKernelBootstrap};

    for (std::size_t declined = 0; declined < sequence.size(); ++declined)
    {
        SCOPED_TRACE(std::format("declining prompt {}", declined));
        auto workflow = FlashWorkflowFactory::tryCreate(bdmWrite(*paths));
        ASSERT_TRUE(workflow != nullptr);
        for (std::size_t accepted = 0; accepted < declined; ++accepted)
        {
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, sequence[accepted]);
            workflow->submit(FlashPromptResponse::Accept);
        }
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, sequence[declined]);
        workflow->submit(FlashPromptResponse::Decline);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Cancelled);
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteWithoutItsKernelFailsBeforeAnyPromptOrIo)
{
    QTemporaryDir without_kernels;
    ASSERT_TRUE(without_kernels.isValid());
    const auto catalog_only = catalogPaths(without_kernels, false);
    ASSERT_TRUE(catalog_only.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectNoBackendIo(*fake);

    for (const bool has_catalog : {true, false})
    {
        SCOPED_TRACE(has_catalog ? "with catalog" : "without catalog");
        auto input = bdmWrite(has_catalog ? *catalog_only : config::ConfigPaths{});
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteKeepsItsKernelSnapshotAfterFilesAreRemoved)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = catalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = FlashWorkflowFactory::tryCreate(bdmWrite(*paths));
    ASSERT_TRUE(workflow != nullptr);

    // The kernel is loaded before Begin; nothing on disk is read afterward.
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    ASSERT_TRUE(QFile::remove(QString::fromStdString(paths->protocols_file)));
    ASSERT_TRUE(QDir(directory.filePath("kernels")).removeRecursively());
    workflow->submit(FlashPromptResponse::Accept);

    std::vector<FlashPromptStep> prompts;
    auto step = acceptEveryPrompt(*workflow, prompts);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << failureDetail(step);
    EXPECT_THAT(promptKinds(prompts), ::testing::ElementsAre(FlashPromptKind::ConfirmBdmKernelBootstrap));
    EXPECT_EQ(std::get<FlashAttempt>(step).attempt->plan().image(),
              std::optional<bytes::Bytes>(bdmCatalogKernelImage()));
}

struct ColtWriteCase
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t capacity;
    std::vector<FlashPromptStep> prompts;
};

std::vector<ColtWriteCase> coltWriteCases()
{
    return {
        {"mitsu_ecu_m32r_can",
         "M32R_384KB_1block",
         0x60000,
         {{FlashPromptKind::Begin, {}},
          {FlashPromptKind::ColtEraseTrigger,
           {{"capacity_kib", "384"}, {"writable_start_hex", "0x8000"}, {"rom_end_hex", "0x60000"}}}}},
        {"mitsu_ecu_m32r_can_vendor_ext_512kb",
         "M32R_512KB_1block",
         0x80000,
         {{FlashPromptKind::Begin, {}},
          {FlashPromptKind::ColtEraseTrigger,
           {{"capacity_kib", "512"}, {"writable_start_hex", "0x8000"}, {"rom_end_hex", "0x80000"}}},
          {FlashPromptKind::ColtTopRegionBootstrap,
           {{"top_region_start_hex", "0x60000"}, {"rom_end_hex", "0x80000"}}}}},
    };
}

FlashWorkflowRequest coltWrite(const ColtWriteCase& test)
{
    auto input = request(std::string(test.protocol), FlashOperation::Write);
    input.mcu = std::string(test.mcu);
    input.image = bytes::Bytes(test.capacity, 0x5a);
    return input;
}

MATCHER_P(IsPrompt, expected, "")
{
    return arg.kind == expected.kind && arg.arguments == expected.arguments;
}

TEST(FlashWorkflowTest, coltWritePromptsCarryTheirCapacityArgumentsInOrder)
{
    for (const ColtWriteCase& test : coltWriteCases())
    {
        SCOPED_TRACE(test.protocol);
        auto workflow = FlashWorkflowFactory::tryCreate(coltWrite(test));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = acceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << failureDetail(step);
        ASSERT_EQ(prompts.size(), test.prompts.size());
        for (std::size_t index = 0; index < prompts.size(); ++index)
        {
            EXPECT_THAT(prompts[index], IsPrompt(test.prompts[index])) << "prompt " << index;
        }

        auto& attempt = std::get<FlashAttempt>(step);
        const FlashPlan& plan = attempt.attempt->plan();
        EXPECT_EQ(plan.family(), FlashFamily::MitsuColtM32rCan);
        EXPECT_EQ(plan.transport(), TransportKind::CanIso15765);
        EXPECT_EQ(plan.operation(), FlashOperation::Write);
        EXPECT_EQ(plan.transfer_region(), (MemoryRegion{0x8000, test.capacity - 0x8000}));
        EXPECT_EQ(plan.image(), std::optional<bytes::Bytes>(bytes::Bytes(test.capacity, 0x5a)));
        EXPECT_EQ(plan.confirmations().size(), test.prompts.size() - 1);

        FakeCancellationToken cancelled(true);
        NullEventSink events;
        EXPECT_THAT(attempt.attempt->run(*attempt.clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::Cancelled, ::testing::HasSubstr("before configure")));

        workflow->submit(FlashAttemptResult{.success = true});
        step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Succeeded);
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).accepted_read_bytes.has_value());
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).rom_id.has_value());
    }
}

TEST(FlashWorkflowTest, coltWriteCancelsWhenAnyPromptIsDeclined)
{
    for (const ColtWriteCase& test : coltWriteCases())
    {
        for (std::size_t declined = 0; declined < test.prompts.size(); ++declined)
        {
            SCOPED_TRACE(std::format("{} declining prompt {}", test.protocol, declined));
            auto workflow = FlashWorkflowFactory::tryCreate(coltWrite(test));
            ASSERT_TRUE(workflow != nullptr);
            for (std::size_t accepted = 0; accepted < declined; ++accepted)
            {
                ASSERT_THAT(std::get<FlashPromptStep>(workflow->next()), IsPrompt(test.prompts[accepted]));
                workflow->submit(FlashPromptResponse::Accept);
            }
            ASSERT_THAT(std::get<FlashPromptStep>(workflow->next()), IsPrompt(test.prompts[declined]));
            workflow->submit(FlashPromptResponse::Decline);
            for (int repeat = 0; repeat < 2; ++repeat)
            {
                const auto step = workflow->next();
                ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
                EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::Cancelled);
            }
        }
    }
}

TEST(FlashWorkflowTest, coltWriteWithWrongImageSizeFailsBeforeAnyPromptOrIo)
{
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    expectNoBackendIo(*fake);

    for (const ColtWriteCase& test : coltWriteCases())
    {
        SCOPED_TRACE(test.protocol);
        auto input = coltWrite(test);
        input.image = bytes::Bytes(test.capacity + 1, 0x5a);
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        const auto step = workflow->next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
    }
}

} // namespace
} // namespace fastecu::flash

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
