#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <gmock/gmock.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "src/backend/calibration/calibration_service.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
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
            !writeFile(kernel_directory + "/catalog_kline_sh7058.bin", QByteArray::fromHex("01020304")))
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
    auto serial = std::make_unique<SerialPortActions>("", "", nullptr, nullptr,
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

class FlashWorkflowTest : public QObject
{
    Q_OBJECT
  private slots:
    void recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone();
    void invalidColtSuffixIsRecognizedButFailsPreflight();
    void preflightPrecedesPromptsAndDeclineCancels();
    void successfulReadBytesAreAcceptedAutomatically();
    void subaruMitsuPropagatesRomId();
    void subaruHitachiRoutesBothModesAndPropagatesReadResult();
    void routesTcuHitachiM32rKlineReadOnly();
    void unisiaJecsRoutesOnlyExactProtocolMcuPairs();
    void unisiaJecsCrossPairsFailBeforeAttempt();
    void routesTcuHitachiM32rCanReadAndWriteRejectsTestWrite();
    void routesSh72543rAliasesAndPreservesImageAndIdentity();
    void routesSh7058ReadAndWriteWithPreTransportPrompts();
    void sh72543rRejectsPreflightAndDeclinedBegin();
    void sh72543rPropagatesFailureAndAbsentIdentity();
    void coltWriteUsesColtSpecificSafetyPrompts();
    void mc68BdmReadRoutesThroughBeginToAttempt();
    void mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom();
    void mc68BdmDeclinedBootstrapConfirmationCancels();
    void mc68BdmTestWriteFailsBeforeAnyPrompt();
    void mc68BdmPrefixLookalikeStaysOffTheKlineFamily();
    void mc68TpuProtocolIsClaimedByPortableRoute();
    void mc68Revision04IsClaimedButPlanBuildFails();
    void sh7055ProtocolIsClaimedByPortableRoute();
    void densoCanRoutesOnlyTheFiveExactProtocols();
    void densoCanResolvesKernelPromptsAndPropagatesAttemptResult();
    void densoCanPreflightAndDeclinedPromptsStopBeforeAttempt();
    void petrolRoutesOnlyTheFiveExactProtocols();
    void petrolSupportedOperationsResolveSecurityAndCatalogKernel();
    void petrolSuccessfulReadPropagatesBytesAndRomId();
    void petrolReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport();
    void dieselRoutesOnlyTheTwoExactProtocols();
    void dieselSupportedOperationsResolveGenerationCatalogKernels();
    void dieselSuccessfulReadPropagatesKernelSnapshotBytesAndRomId();
    void dieselReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport();
    void tcuRoutesOnlyTheTwoExactProtocols();
    void tcuSupportedOperationsResolveTheirCatalogKernelAndReachAttempt();
    void tcuUnsupportedOperationsFailBeforeTransportIo();
    void tcuReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport();
    void tcuSuccessfulReadPropagatesBytesAndRomId();
    void mc68ResolvesKernelThroughCatalogBeforePromptAndAttempt();
    void missingCatalogKernelFailsBeforePrompt();
    void sh7055IteratesConfirmationsAndPropagatesAttemptResult();
    void sh7055EcutekResolvesWithoutCarModelReference();
    void portableImageCopiesRomForEveryNonReadOperation();
    void mc68TestWriteWithPortableImageReachesAttempt();
    void mc68PhysicalImageIsPackedAtWorkflowBoundary();
    void mc68CalibrationPaddingRoundTripsToPackedWriteImage();
    void sh7055TestWriteWithPortableImageReachesPromptsAndAttempt();
    void mc68TpuReadResolvesCatalogAndReachesAttempt();
    void densoSh705xKlineRoutesExactProtocolsThroughBeginToAttempt();
    void densoSh705xKlineCobbReadFailsBeforeAttempt();
    void densoSh705xKlineIgnoresPrefixLookalikes();
};

void FlashWorkflowTest::recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone()
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
                                                                  "sub_ecu_denso_mc68hc16y5_02_bdm"});
    for (const char *protocol : portable)
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr, protocol);
    }
}

void FlashWorkflowTest::invalidColtSuffixIsRecognizedButFailsPreflight()
{
    auto workflow = FlashWorkflowFactory::tryCreate(request("mitsu_ecu_m32r_can_typo"));
    QVERIFY(workflow != nullptr);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

void FlashWorkflowTest::preflightPrecedesPromptsAndDeclineCancels()
{
    auto invalid = request("mitsu_ecu_m32r_can", FlashOperation::TestWrite);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(invalid));
    QVERIFY(std::holds_alternative<FlashFailureStep>(workflow->next()));

    workflow = FlashWorkflowFactory::tryCreate(request("mitsu_ecu_m32r_can"));
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

void FlashWorkflowTest::successfulReadBytesAreAcceptedAutomatically()
{
    auto workflow = FlashWorkflowFactory::tryCreate(request("mitsu_ecu_m32r_can"));
    (void)workflow->next();
    workflow->submit(FlashPromptResponse::Accept);
    QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1, 2, 3}});
    auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({1, 2, 3}));
}

void FlashWorkflowTest::unisiaJecsRoutesOnlyExactProtocolMcuPairs()
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
        QVERIFY2(workflow != nullptr, protocol);
        QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        auto step = workflow->next();
        QVERIFY(std::holds_alternative<FlashAttempt>(step));
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        QCOMPARE(plan.family(), FlashFamily::SubaruUnisiaJecs);
        QCOMPARE(plan.transport(), TransportKind::Kline);
        QCOMPARE(plan.target_id(), protocol);
        QCOMPARE(plan.mcu_name(), mcu);
    }

    QVERIFY(FlashWorkflowFactory::tryCreate(request("sub_ecu_unisia_jecs_m3779x_suffix")) == nullptr);
    QVERIFY(FlashWorkflowFactory::tryCreate(request("sub_ecu_unisia_jecs_m3775x_suffix")) == nullptr);
}

void FlashWorkflowTest::unisiaJecsCrossPairsFailBeforeAttempt()
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
        QVERIFY2(workflow != nullptr, protocol);
        auto step = workflow->next();
        QVERIFY(std::holds_alternative<FlashFailureStep>(step));
        QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
    }
}

void FlashWorkflowTest::subaruMitsuPropagatesRomId()
{
    auto input = request("sub_ecu_mitsu_m32r_kline");
    input.mcu = "M32R_512KB_4blocks";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0xff, 0x12}, .rom_id = "123456789A_"});
    auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
}

void FlashWorkflowTest::subaruHitachiRoutesBothModesAndPropagatesReadResult()
{
    for (const char *protocol : {"sub_ecu_hitachi_m32r_kline", "sub_ecu_hitachi_m32r_kline_recovery"})
    {
        auto input = request(protocol);
        input.mcu = "M32R_512KB_1block";
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY(workflow != nullptr);
        QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
        workflow->submit(
            FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
        auto done = workflow->next();
        QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
        QCOMPARE(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5a}));
        QCOMPARE(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
    }
}

void FlashWorkflowTest::routesTcuHitachiM32rKlineReadOnly()
{
    auto input = request("sub_tcu_hitachi_m32r_kline");
    input.mcu = "M32R_512KB";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5a}));
    QCOMPARE(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));

    // Write is rejected by the plan builder (the family is read-only), so the
    // workflow's very first step must be a failure rather than a prompt or an
    // attempt -- the legacy path silently "succeeded" while writing nothing.
    auto write_request = request("sub_tcu_hitachi_m32r_kline");
    write_request.mcu = "M32R_512KB";
    write_request.operation = FlashOperation::Write;
    write_request.image = bytes::Bytes(0x80000, 0x00);
    auto write_workflow = FlashWorkflowFactory::tryCreate(std::move(write_request));
    QVERIFY(write_workflow != nullptr);
    const auto write_step = write_workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(write_step));
    QCOMPARE(std::get<FlashFailureStep>(write_step).error.kind, ErrorKind::Unsupported);
}

void FlashWorkflowTest::routesTcuHitachiM32rCanReadAndWriteRejectsTestWrite()
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
    QVERIFY(read_workflow != nullptr);
    QCOMPARE(std::get<FlashPromptStep>(read_workflow->next()).kind, FlashPromptKind::Begin);
    read_workflow->submit(FlashPromptResponse::Accept);
    const auto read_step = read_workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(read_step));
    const FlashPlan& read_plan = std::get<FlashAttempt>(read_step).attempt->plan();
    QCOMPARE(read_plan.target_id(), std::string_view(kProtocol));
    QVERIFY(read_plan.operation() == FlashOperation::Read);
    QCOMPARE(read_plan.transport(), TransportKind::CanIso15765);
    read_workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}});
    const auto read_done = read_workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(read_done));
    QCOMPARE(std::get<FlashCompletedStep>(read_done).outcome, FlashWorkflowOutcome::Succeeded);
    QCOMPARE(std::get<FlashCompletedStep>(read_done).accepted_read_bytes, bytes::Bytes({0x5a}));

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
    QVERIFY(test_write_workflow != nullptr);
    const auto test_write_step = test_write_workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(test_write_step));
    QCOMPARE(std::get<FlashFailureStep>(test_write_step).error.kind, ErrorKind::Unsupported);

    // Write, unlike the K-Line sibling, is supported by this family and
    // routes all the way to an attempt bound to the CAN executor/transport.
    auto write_input = request(kProtocol, FlashOperation::Write);
    write_input.mcu = kMcu;
    write_input.image = bytes::Bytes(0x80000, 0xa5);
    auto write_workflow = FlashWorkflowFactory::tryCreate(std::move(write_input));
    QVERIFY(write_workflow != nullptr);
    QCOMPARE(std::get<FlashPromptStep>(write_workflow->next()).kind, FlashPromptKind::Begin);
    write_workflow->submit(FlashPromptResponse::Accept);
    const auto write_step = write_workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(write_step));
    const FlashPlan& write_plan = std::get<FlashAttempt>(write_step).attempt->plan();
    QCOMPARE(write_plan.target_id(), std::string_view(kProtocol));
    QVERIFY(write_plan.operation() == FlashOperation::Write);
    QCOMPARE(write_plan.transport(), TransportKind::CanIso15765);
}

void FlashWorkflowTest::routesSh72543rAliasesAndPreservesImageAndIdentity()
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
            QVERIFY(workflow);
            QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
            workflow->submit(FlashPromptResponse::Accept);
            auto step = workflow->next();
            QVERIFY(std::holds_alternative<FlashAttempt>(step));
            const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
            QCOMPARE(plan.family(), FlashFamily::SubaruHitachiSh72543rCan);
            QCOMPARE(plan.target_id(), std::string_view(protocol));
            QCOMPARE(plan.transport(), TransportKind::CanIso15765);
            QCOMPARE(plan.transfer_region().start, operation == FlashOperation::Read ? 0U : 0x6000U);
            if (operation == FlashOperation::Write)
            {
                QCOMPARE(*plan.image(), bytes::Bytes(0x200000, 0xa5));
            }
            workflow->submit(FlashAttemptResult{
                .success = true,
                .read_bytes = operation == FlashOperation::Read ? std::optional{bytes::Bytes{1, 2, 3}} : std::nullopt,
                .rom_id =
                    operation == FlashOperation::Read ? std::optional<std::string>{"CAL_1122334455_"} : std::nullopt});
            auto done = std::get<FlashCompletedStep>(workflow->next());
            QCOMPARE(done.outcome, FlashWorkflowOutcome::Succeeded);
            if (operation == FlashOperation::Read)
            {
                QCOMPARE(done.accepted_read_bytes, bytes::Bytes({1, 2, 3}));
                QCOMPARE(done.rom_id, std::string("CAL_1122334455_"));
            }
            else
            {
                QVERIFY(!done.accepted_read_bytes);
                QVERIFY(!done.rom_id);
            }
        }
    }
    QVERIFY(!FlashWorkflowFactory::tryCreate(request("sub_ecu_hitachi_sh72543r_can_recovery_typo")));
    QVERIFY(!FlashWorkflowFactory::tryCreate(request("sub_ecu_hitachi_sh72543r_can_typo")));
}
void FlashWorkflowTest::routesSh7058ReadAndWriteWithPreTransportPrompts()
{
    QVERIFY(!FlashWorkflowFactory::tryCreate(request("sub_ecu_hitachi_sh7058_can_extra")));
    for (const auto operation : {FlashOperation::Read, FlashOperation::Write})
    {
        auto input = request("sub_ecu_hitachi_sh7058_can", operation);
        input.mcu = "SH7058_1block";
        if (operation == FlashOperation::Write)
        {
            input.image = bytes::Bytes(0x100000, 0x5a);
        }
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY(workflow);
        QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        if (operation == FlashOperation::Read)
        {
            QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ConfirmSh7058Read);
            workflow->submit(FlashPromptResponse::Accept);
        }
        auto step = workflow->next();
        QVERIFY(std::holds_alternative<FlashAttempt>(step));
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        QCOMPARE(plan.family(), FlashFamily::SubaruHitachiSh7058);
        QCOMPARE(plan.transport(),
                 operation == FlashOperation::Read ? TransportKind::Kline : TransportKind::CanIso15765);
    }
    auto input = request("sub_ecu_hitachi_sh7058_can");
    input.mcu = "SH7058_1block";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    workflow->submit(FlashPromptResponse::Accept);
    workflow->submit(FlashPromptResponse::Decline);
    QCOMPARE(std::get<FlashCompletedStep>(workflow->next()).outcome, FlashWorkflowOutcome::Cancelled);
}
void FlashWorkflowTest::sh72543rRejectsPreflightAndDeclinedBegin()
{
    for (const char *protocol : {"sub_ecu_hitachi_sh72543r_can", "sub_ecu_hitachi_sh72543r_can_recovery"})
    {
        for (int fault = 0; fault < 3; ++fault)
        {
            auto input = request(protocol, fault == 0 ? FlashOperation::TestWrite : FlashOperation::Write);
            input.mcu = fault == 1 ? "SH72543d" : "SH72543R";
            input.image = bytes::Bytes(fault == 2 ? 16 : 0x200000);
            auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
            QVERIFY(workflow);
            auto step = workflow->next();
            QVERIFY(std::holds_alternative<FlashFailureStep>(step));
            QCOMPARE(std::get<FlashFailureStep>(step).error.kind,
                     fault == 0 ? ErrorKind::Unsupported : ErrorKind::InvalidConfig);
        }
        auto input = request(protocol);
        input.mcu = "SH72543R";
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY(workflow);
        QVERIFY(std::holds_alternative<FlashPromptStep>(workflow->next()));
        workflow->submit(FlashPromptResponse::Decline);
        auto done = std::get<FlashCompletedStep>(workflow->next());
        QCOMPARE(done.outcome, FlashWorkflowOutcome::Cancelled);
        QVERIFY(!done.accepted_read_bytes);
    }
}
void FlashWorkflowTest::sh72543rPropagatesFailureAndAbsentIdentity()
{
    for (int outcome = 0; outcome < 3; ++outcome)
    {
        auto input = request("sub_ecu_hitachi_sh72543r_can");
        input.mcu = "SH72543R";
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY(workflow);
        workflow->submit(FlashPromptResponse::Accept);
        QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
        workflow->submit(
            FlashAttemptResult{.success = outcome == 0,
                               .error_kind = outcome == 1 ? ErrorKind::Disconnected : ErrorKind::Cancelled,
                               .error_detail = "lost adapter",
                               .read_bytes = outcome == 0 ? std::optional{bytes::Bytes{4, 5}} : std::nullopt});
        auto step = workflow->next();
        if (outcome == 1)
        {
            QVERIFY(std::holds_alternative<FlashFailureStep>(step));
            QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Disconnected);
        }
        else
        {
            auto done = std::get<FlashCompletedStep>(step);
            QVERIFY(!done.rom_id);
            QCOMPARE(done.outcome, outcome == 0 ? FlashWorkflowOutcome::Succeeded : FlashWorkflowOutcome::Cancelled);
        }
    }
}

void FlashWorkflowTest::coltWriteUsesColtSpecificSafetyPrompts()
{
    auto write = request("mitsu_ecu_m32r_can", FlashOperation::Write);
    write.image = bytes::Bytes(0x60000);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(write));

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ColtEraseTrigger);
}

void FlashWorkflowTest::mc68BdmReadRoutesThroughBeginToAttempt()
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm");
    input.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    QCOMPARE(plan.family(), FlashFamily::SubaruDensoMc68hc16y5_02Bdm);
    QCOMPARE(plan.transport(), TransportKind::Kline);
    QCOMPARE(plan.transfer_region(), (MemoryRegion{0, 0x30000}));
    QVERIFY(!plan.image().has_value());
}

void FlashWorkflowTest::mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    input.image = bytes::Bytes(0x30000, 0x5a);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        QFAIL(failure->error.detail.c_str());
    }
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ConfirmBdmKernelBootstrap);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    bytes::Bytes expected(0x20, 0x00);
    expected[0] = 0x11;
    expected[1] = 0x22;
    expected[2] = 0x33;
    QCOMPARE(plan.image(), std::optional<bytes::Bytes>(expected));
    QCOMPARE(plan.transfer_region(), (MemoryRegion{0x20000, 0x20}));
    QVERIFY(!plan.kernel().has_value());
}

void FlashWorkflowTest::mc68BdmDeclinedBootstrapConfirmationCancels()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ConfirmBdmKernelBootstrap);
    workflow->submit(FlashPromptResponse::Decline);
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
}

void FlashWorkflowTest::mc68BdmTestWriteFailsBeforeAnyPrompt()
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::TestWrite);
    input.mcu = "MC68HC16Y5";
    input.image = bytes::Bytes(0x30000, 0x5a);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

void FlashWorkflowTest::mc68BdmPrefixLookalikeStaysOffTheKlineFamily()
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm_x");
    input.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

void FlashWorkflowTest::mc68TpuProtocolIsClaimedByPortableRoute()
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_tpu");
    input.mcu = "MC68HC16Y5_TPU";
    QVERIFY(FlashWorkflowFactory::tryCreate(std::move(input)) != nullptr);
}

void FlashWorkflowTest::mc68Revision04IsClaimedButPlanBuildFails()
{
    auto input = request("sub_ecu_denso_mc68hc16y5_04");
    input.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

void FlashWorkflowTest::sh7055ProtocolIsClaimedByPortableRoute()
{
    auto input = request("sub_ecu_denso_sh7055_02");
    input.mcu = "SH7055";
    QVERIFY(FlashWorkflowFactory::tryCreate(std::move(input)) != nullptr);
}

void FlashWorkflowTest::densoCanRoutesOnlyTheFiveExactProtocols()
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
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr, protocol);
    }
    for (const char *near_miss : {"sub_ecu_denso_sh7058_densocan_extra", "future_densocan"})
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr, near_miss);
    }
}

void FlashWorkflowTest::densoCanResolvesKernelPromptsAndPropagatesAttemptResult()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_sh7055_densocan");
    input.mcu = "SH7055";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    QVERIFY(QFile::remove(QString::fromStdString(paths->protocols_file)));
    QVERIFY(QFile::remove(directory.filePath("kernels/catalog_densocan.bin")));
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::CycleIgnition);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    QCOMPARE(plan.transport(), TransportKind::CanRawIso15765);
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->bytes, bytes::Bytes({0xaa, 0xbb, 0xcc, 0xdd}));

    workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(step));
    const auto& done = std::get<FlashCompletedStep>(step);
    QCOMPARE(done.outcome, FlashWorkflowOutcome::Succeeded);
    QCOMPARE(done.accepted_read_bytes, bytes::Bytes({0x5a}));
    QCOMPARE(done.rom_id, std::string("123456789A_"));
}

void FlashWorkflowTest::densoCanPreflightAndDeclinedPromptsStopBeforeAttempt()
{
    auto missing_catalog = request("sub_ecu_denso_sh7055_densocan");
    missing_catalog.mcu = "SH7055";
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(missing_catalog));
    QVERIFY(workflow != nullptr);
    QVERIFY(std::holds_alternative<FlashFailureStep>(workflow->next()));

    for (const bool decline_begin : {true, false})
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto input = request("sub_ecu_denso_sh7055_densocan");
        input.mcu = "SH7055";
        const auto paths = catalogPaths(directory);
        QVERIFY(paths.has_value());
        input.paths = *paths;
        workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY(workflow != nullptr);
        QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        if (decline_begin)
        {
            workflow->submit(FlashPromptResponse::Decline);
        }
        else
        {
            workflow->submit(FlashPromptResponse::Accept);
            QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::CycleIgnition);
            workflow->submit(FlashPromptResponse::Decline);
        }
        const auto done = workflow->next();
        QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
        QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Cancelled);
    }
}

void FlashWorkflowTest::petrolRoutesOnlyTheFiveExactProtocols()
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
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr, protocol);
    }
    for (const char *near_miss : {
             "sub_ecu_denso_sh7058_can_future",
             "sub_ecu_denso_sh7058_can_ecutek_extra",
             "sub_ecu_denso_sh7058_can_cobb_typo",
         })
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr, near_miss);
    }
}

void FlashWorkflowTest::petrolSupportedOperationsResolveSecurityAndCatalogKernel()
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
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());

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
        QVERIFY2(workflow != nullptr, test.protocol);

        auto step = workflow->next();
        if (const auto *failure = std::get_if<FlashFailureStep>(&step))
        {
            QFAIL(failure->error.detail.c_str());
        }
        QVERIFY(std::holds_alternative<FlashPromptStep>(step));
        QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        step = workflow->next();
        QVERIFY(std::holds_alternative<FlashAttempt>(step));
        const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->plan();
        QCOMPARE(plan.family(), FlashFamily::SubaruDensoSh7058Can);
        QCOMPARE(plan.transport(), TransportKind::CanIso15765);
        QCOMPARE(plan.target_id(), std::string_view(test.protocol));
        QCOMPARE(plan.mcu_name(), std::string_view("SH7058"));
        QCOMPARE(plan.operation(), test.operation);
        QVERIFY(plan.confirmations().empty());
        QVERIFY(plan.kernel().has_value());
        QCOMPARE(plan.kernel()->load_address, 0xFFFF3000U);
        QCOMPARE(plan.kernel()->bytes, bytes::Bytes({0x90, 0xA0, 0xB0, 0xC0}));
        const auto *family_plan = std::get_if<SubaruDensoSh7058CanPlan>(&plan.family_plan());
        QVERIFY(family_plan != nullptr);
        QCOMPARE(family_plan->request_id, 0x7E0U);
        QCOMPARE(family_plan->response_id, 0x7E8U);
        QCOMPARE(family_plan->bitrate, 500000);
        QVERIFY(!family_plan->extended_id);
        QCOMPARE(family_plan->security, test.security);
    }
}

void FlashWorkflowTest::petrolSuccessfulReadPropagatesBytesAndRomId()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_sh7058_can");
    input.mcu = "SH7058";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5A, 0xA5}, .rom_id = "CALID_123456789A_"});
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
    QCOMPARE(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5A, 0xA5}));
    QCOMPARE(std::get<FlashCompletedStep>(done).rom_id, std::string("CALID_123456789A_"));
}

void FlashWorkflowTest::petrolReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    QVERIFY(serial != nullptr);
    expectCanTransportSetup(*fake, true, 2016, 2024);

    auto input = request("sub_ecu_denso_sh7058_can");
    input.mcu = "SH7058";
    input.paths = *paths;
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    // Resolution happened before Begin; removing the catalog and kernel now
    // must not affect the already-bound attempt.
    QVERIFY(QFile::remove(QString::fromStdString(paths->protocols_file)));
    QVERIFY(QFile::remove(directory.filePath("kernels/catalog_petrol_sh7058.bin")));

    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->plan();
    QCOMPARE(plan.family(), FlashFamily::SubaruDensoSh7058Can);
    QCOMPARE(plan.transport(), TransportKind::CanIso15765);
    QCOMPARE(plan.target_id(), std::string_view("sub_ecu_denso_sh7058_can"));
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->bytes, bytes::Bytes({0x90, 0xA0, 0xB0, 0xC0}));

    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(5);
    NullEventSink events;
    const auto result = attempt.attempt->run(*attempt.clock, cancellation, events);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Cancelled);
}

void FlashWorkflowTest::dieselRoutesOnlyTheTwoExactProtocols()
{
    for (const char *protocol : {"sub_ecu_denso_sh7058_can_diesel", "sub_ecu_denso_sh7059_can_diesel"})
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr, protocol);
    }
    for (const char *near_miss : {"sub_ecu_denso_sh7058_can_diesel_future", "sub_ecu_denso_sh7059_can_diesel_extra",
                                  "sub_ecu_denso_sh7058_can_diesel_typo", "sub_ecu_denso_sh7058_can_diesel_ecutek"})
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr, near_miss);
    }
}

void FlashWorkflowTest::dieselSupportedOperationsResolveGenerationCatalogKernels()
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
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
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
        QVERIFY2(workflow != nullptr, test.protocol);
        auto step = workflow->next();
        if (const auto *failure = std::get_if<FlashFailureStep>(&step))
        {
            QFAIL(failure->error.detail.c_str());
        }
        QVERIFY(std::holds_alternative<FlashPromptStep>(step));
        QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        step = workflow->next();
        QVERIFY(std::holds_alternative<FlashAttempt>(step));
        const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->plan();
        QCOMPARE(plan.family(), FlashFamily::SubaruDensoSh7058CanDiesel);
        QCOMPARE(plan.transport(), TransportKind::CanIso15765);
        QCOMPARE(plan.target_id(), std::string_view(test.protocol));
        QCOMPARE(plan.mcu_name(), std::string_view(test.mcu));
        QCOMPARE(plan.operation(), test.operation);
        QCOMPARE(plan.transfer_region().length, static_cast<std::uint32_t>(test.rom_size));
        QVERIFY(plan.confirmations().empty());
        QVERIFY(plan.kernel().has_value());
        QCOMPARE(plan.kernel()->load_address, test.kernel_address);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(plan.kernel()->bytes.data()),
                            static_cast<int>(plan.kernel()->bytes.size())),
                 test.kernel_bytes);
        const auto *family_plan = std::get_if<SubaruDensoSh7058CanDieselPlan>(&plan.family_plan());
        QVERIFY(family_plan != nullptr);
        QCOMPARE(family_plan->request_id, 0x7E0U);
        QCOMPARE(family_plan->response_id, 0x7E8U);
        QCOMPARE(family_plan->bitrate, 500000);
        QVERIFY(!family_plan->extended_id);
    }
}

void FlashWorkflowTest::dieselSuccessfulReadPropagatesKernelSnapshotBytesAndRomId()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_sh7059_can_diesel");
    input.mcu = "SH7059d";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    QVERIFY(QFile::remove(QString::fromStdString(paths->protocols_file)));
    QVERIFY(QFile::remove(directory.filePath("kernels/catalog_diesel_sh7059.bin")));
    workflow->submit(FlashPromptResponse::Accept);
    const auto attempt_step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(attempt_step));
    const FlashPlan& snapshot = std::get<FlashAttempt>(attempt_step).attempt->plan();
    QVERIFY(snapshot.kernel().has_value());
    QCOMPARE(snapshot.kernel()->load_address, 0xFFFEE000U);
    QCOMPARE(snapshot.kernel()->bytes, bytes::Bytes({0xD0, 0xE0, 0xF0, 0x02}));

    workflow->submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0xD1, 0xE5}, .rom_id = "DIESEL_CAL_ECU_"});
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
    QCOMPARE(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0xD1, 0xE5}));
    QCOMPARE(std::get<FlashCompletedStep>(done).rom_id, std::string("DIESEL_CAL_ECU_"));
}

void FlashWorkflowTest::dieselReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    QVERIFY(serial != nullptr);
    expectCanTransportSetup(*fake, true, 2016, 2024);

    auto input = request("sub_ecu_denso_sh7059_can_diesel");
    input.mcu = "SH7059d";
    input.paths = *paths;
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);

    // The workflow owns resolved catalog data before Begin; this also proves
    // the Diesel route is a real DesktopCan attempt rather than a legacy
    // MainWindow branch.
    QVERIFY(QFile::remove(QString::fromStdString(paths->protocols_file)));
    QVERIFY(QFile::remove(directory.filePath("kernels/catalog_diesel_sh7059.bin")));
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->plan();
    QCOMPARE(plan.family(), FlashFamily::SubaruDensoSh7058CanDiesel);
    QCOMPARE(plan.transport(), TransportKind::CanIso15765);
    QCOMPARE(plan.target_id(), std::string_view("sub_ecu_denso_sh7059_can_diesel"));
    QCOMPARE(plan.mcu_name(), std::string_view("SH7059d"));
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->load_address, 0xFFFEE000U);
    QCOMPARE(plan.kernel()->bytes, bytes::Bytes({0xD0, 0xE0, 0xF0, 0x02}));

    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(53);
    NullEventSink events;
    const auto result = attempt.attempt->run(*attempt.clock, cancellation, events);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Cancelled);
}

void FlashWorkflowTest::tcuRoutesOnlyTheTwoExactProtocols()
{
    for (const char *protocol : {"sub_tcu_denso_sh7055_can", "sub_tcu_denso_sh7058_can"})
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr, protocol);
    }
    for (const char *near_miss :
         {"sub_tcu_denso_sh7055_can_future", "sub_tcu_denso_sh7058_can_typo", "sub_tcu_denso_sh7058_can_extra"})
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr, near_miss);
    }
}

void FlashWorkflowTest::tcuSupportedOperationsResolveTheirCatalogKernelAndReachAttempt()
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
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());

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
        QVERIFY2(workflow != nullptr, test.protocol);

        auto step = workflow->next();
        if (const auto *failure = std::get_if<FlashFailureStep>(&step))
        {
            QFAIL(failure->error.detail.c_str());
        }
        QVERIFY(std::holds_alternative<FlashPromptStep>(step));
        QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        step = workflow->next();
        QVERIFY(std::holds_alternative<FlashAttempt>(step));
        const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->plan();
        QCOMPARE(plan.target_id(), std::string_view(test.protocol));
        QVERIFY(plan.operation() == test.operation);
        QCOMPARE(plan.transport(), TransportKind::CanIso15765);
        QVERIFY(plan.kernel().has_value());
        QCOMPARE(plan.kernel()->load_address, test.kernel_address);
        QCOMPARE(plan.kernel()->bytes, test.kernel_bytes);
    }
}

void FlashWorkflowTest::tcuUnsupportedOperationsFailBeforeTransportIo()
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
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    QVERIFY(serial != nullptr);
    expectNoBackendIo(*fake);

    for (const Case& test : cases)
    {
        auto input = request(test.protocol, test.operation);
        input.mcu = test.mcu;
        input.paths = *paths;
        input.image = bytes::Bytes(test.image_size, 0xa5);
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY2(workflow != nullptr, test.protocol);

        const auto step = workflow->next();
        QVERIFY(std::holds_alternative<FlashFailureStep>(step));
        QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
    }
}

void FlashWorkflowTest::tcuReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = recordingSerial(&fake);
    QVERIFY(serial != nullptr);
    expectCanTransportSetup(*fake, false, 2017, 2025);

    auto input = request("sub_tcu_denso_sh7055_can");
    input.mcu = "SH7055";
    input.paths = *paths;
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    QVERIFY(QFile::remove(QString::fromStdString(paths->protocols_file)));
    QVERIFY(QFile::remove(directory.filePath("kernels/catalog_tcu_sh7055.bin")));

    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->plan();
    QCOMPARE(plan.transport(), TransportKind::CanIso15765);
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->bytes, bytes::Bytes({0x10, 0x20, 0x30, 0x40}));

    FakeCancellationToken cancellation;
    cancellation.cancel_on_check(2);
    NullEventSink events;
    const auto result = attempt.attempt->run(*attempt.clock, cancellation, events);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Cancelled);
}

void FlashWorkflowTest::tcuSuccessfulReadPropagatesBytesAndRomId()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto input = request("sub_tcu_denso_sh7058_can");
    input.mcu = "SH7058";
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
    workflow->submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a, 0xa5}, .rom_id = "123456789A_"});
    const auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::Succeeded);
    QCOMPARE(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5a, 0xa5}));
    QCOMPARE(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
}

void FlashWorkflowTest::mc68ResolvesKernelThroughCatalogBeforePromptAndAttempt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02");
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        QFAIL(failure->error.detail.c_str());
    }
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);

    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->load_address, 0x20000U);
    QCOMPARE(plan.kernel()->bytes, bytes::Bytes({0x11, 0x22, 0x33}));
}

void FlashWorkflowTest::missingCatalogKernelFailsBeforePrompt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02");
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory, false);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    const auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::InvalidConfig);
}

void FlashWorkflowTest::sh7055IteratesConfirmationsAndPropagatesAttemptResult()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_sh7055_02");
    input.mcu = "SH7055";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::CycleIgnition);
    workflow->submit(FlashPromptResponse::Accept);

    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->load_address, 0xFFFF6004U);
    QCOMPARE(plan.kernel()->bytes, bytes::Bytes({0xaa, 0xbb, 0xcc, 0xdd}));

    workflow->submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(step));
    const auto& done = std::get<FlashCompletedStep>(step);
    QCOMPARE(done.outcome, FlashWorkflowOutcome::Succeeded);
    QCOMPARE(done.accepted_read_bytes, bytes::Bytes({0x5a}));
    QCOMPARE(done.rom_id, std::string("123456789A_"));
}

void FlashWorkflowTest::sh7055EcutekResolvesWithoutCarModelReference()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_sh7055_02_ecutek");
    input.mcu = "SH7055";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashPromptStep>(step));
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
    QCOMPARE(plan.target_id(), std::string_view("sub_ecu_denso_sh7055_02_ecutek"));
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->load_address, 0xFFFF6004U);
}

void FlashWorkflowTest::portableImageCopiesRomForEveryNonReadOperation()
{
    const bytes::Bytes rom{0x11, 0x22};
    QVERIFY(!portableImageForOperation(FlashOperation::Read, rom).has_value());
    QCOMPARE(portableImageForOperation(FlashOperation::Write, rom), rom);
    QCOMPARE(portableImageForOperation(FlashOperation::TestWrite, rom), rom);
}

void FlashWorkflowTest::mc68TestWriteWithPortableImageReachesAttempt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02", FlashOperation::TestWrite);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    const bytes::Bytes packed_image(0x28000, 0x5a);
    input.image = portableImageForOperation(input.operation, packed_image);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    QCOMPARE(std::get<FlashAttempt>(step).attempt->plan().image(), packed_image);
}

void FlashWorkflowTest::mc68PhysicalImageIsPackedAtWorkflowBoundary()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02", FlashOperation::Write);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;

    bytes::Bytes physical_image(0x30000, 0xee);
    std::fill_n(physical_image.begin(), 0x20000, 0x11);
    std::fill(physical_image.begin() + 0x28000, physical_image.end(), 0x22);
    input.image = physical_image;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    auto step = workflow->next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        QFAIL(failure->error.detail.c_str());
    }
    QCOMPARE(std::get<FlashPromptStep>(step).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& packed = std::get<FlashAttempt>(step).attempt->plan().image();
    QVERIFY(packed.has_value());
    QCOMPARE(packed->size(), std::size_t{0x28000});
    QVERIFY(std::all_of(packed->begin(), packed->begin() + 0x20000, [](bytes::Byte value) { return value == 0x11; }));
    QVERIFY(std::all_of(packed->begin() + 0x20000, packed->end(), [](bytes::Byte value) { return value == 0x22; }));
}

void FlashWorkflowTest::mc68CalibrationPaddingRoundTripsToPackedWriteImage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02", FlashOperation::TestWrite);
    input.mcu = "MC68HC16Y5";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;

    bytes::Bytes packed_image(0x28000);
    for (std::size_t index = 0; index < packed_image.size(); ++index)
    {
        packed_image[index] = static_cast<bytes::Byte>((index / 0x4000) + 1);
    }
    input.image = calibration::apply_flash_method_padding(packed_image, "sub_ecu_denso_mc68hc16y5_02");
    QCOMPARE(input.image->size(), std::size_t{0x30000});

    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    QCOMPARE(std::get<FlashAttempt>(step).attempt->plan().image(), packed_image);
}

void FlashWorkflowTest::sh7055TestWriteWithPortableImageReachesPromptsAndAttempt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_sh7055_02", FlashOperation::TestWrite);
    input.mcu = "SH7055";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    input.image = portableImageForOperation(input.operation, bytes::Bytes(0x80000, 0xa5));
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::CycleIgnition);
    workflow->submit(FlashPromptResponse::Accept);
    QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
}

void FlashWorkflowTest::mc68TpuReadResolvesCatalogAndReachesAttempt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto input = request("sub_ecu_denso_mc68hc16y5_02_tpu");
    input.mcu = "MC68HC16Y5_TPU";
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    const auto& kernel = std::get<FlashAttempt>(step).attempt->plan().kernel();
    QVERIFY(kernel.has_value());
    QCOMPARE(kernel->load_address, 0x20000U);
    QCOMPARE(kernel->bytes, bytes::Bytes({0x44, 0x55, 0x66}));
}

void FlashWorkflowTest::densoSh705xKlineRoutesExactProtocolsThroughBeginToAttempt()
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
        QVERIFY(directory.isValid());
        const auto paths = catalogPaths(directory);
        QVERIFY(paths.has_value());
        auto input = request(c.protocol, c.operation);
        input.mcu = c.mcu;
        input.paths = *paths;
        if (c.operation != FlashOperation::Read)
        {
            input.image = bytes::Bytes(std::size_t{1024} * 1024, 0xff);
        }
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY2(workflow != nullptr, c.protocol);

        QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        auto step = workflow->next();
        QVERIFY2(std::holds_alternative<FlashAttempt>(step), c.protocol);
        const auto& plan = std::get<FlashAttempt>(step).attempt->plan();
        QCOMPARE(plan.family(), FlashFamily::SubaruDensoSh705xKline);
        QCOMPARE(plan.target_id(), std::string(c.protocol));
        QVERIFY(plan.kernel().has_value());
        QCOMPARE(plan.kernel()->bytes, c.kernel);
        QCOMPARE(std::get<SubaruDensoSh705xKlinePlan>(plan.family_plan()).seed_key, c.seed_key);
    }
}

void FlashWorkflowTest::densoSh705xKlineCobbReadFailsBeforeAttempt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto paths = catalogPaths(directory);
    QVERIFY(paths.has_value());
    auto input = request("sub_ecu_denso_sh7058_cobb");
    input.mcu = "SH7058";
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashFailureStep>(step));
    QCOMPARE(std::get<FlashFailureStep>(step).error.kind, ErrorKind::Unsupported);
}

void FlashWorkflowTest::densoSh705xKlineIgnoresPrefixLookalikes()
{
    for (const char *near_miss :
         {"sub_ecu_denso_sh7055_04_future", "sub_ecu_denso_sh7058_extra", "sub_ecu_denso_sh7058_ecutek_racerom"})
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr, near_miss);
    }
}

} // namespace
} // namespace fastecu::flash

QTEST_MAIN(fastecu::flash::FlashWorkflowTest)
#include "flash_workflow_test.moc"
