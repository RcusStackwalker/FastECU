#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <memory>

#include "src/backend/calibration/calibration_service.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
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
            !writeFile(kernel_directory + "/catalog_petrol_sh7058.bin", QByteArray::fromHex("90a0b0c0")))
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
                                                          *fake = new FakeBackend;
                                                          return *fake;
                                                      });
    if (!serial->set_add_ssm_header(false) || *fake == nullptr)
    {
        return nullptr;
    }
    (*fake)->takeCallLog();
    return serial;
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
    void coltWriteUsesColtSpecificSafetyPrompts();
    void mc68BdmProtocolIsNotClaimedByPortableRoute();
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
                                                                  "sub_tcu_cvt_hitachi_m32r_can",
                                                                  "sub_tcu_cvt_mitsu_mh8111_can",
                                                                  "sub_tcu_cvt_mitsu_mh8104_can",
                                                                  "sub_ecu_denso_1n83m_1_5m_can",
                                                                  "sub_ecu_denso_sh72531_can",
                                                                  "sub_ecu_denso_sh72543_can_diesel",
                                                                  "sub_ecu_denso_1n83m_4m_can"});
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

void FlashWorkflowTest::coltWriteUsesColtSpecificSafetyPrompts()
{
    auto write = request("mitsu_ecu_m32r_can", FlashOperation::Write);
    write.image = bytes::Bytes(0x60000);
    auto workflow = FlashWorkflowFactory::tryCreate(std::move(write));

    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::ColtEraseTrigger);
}

void FlashWorkflowTest::mc68BdmProtocolIsNotClaimedByPortableRoute()
{
    auto input = request("sub_ecu_denso_mc68hc16y5_02_bdm");
    input.mcu = "MC68HC16Y5";
    QVERIFY(FlashWorkflowFactory::tryCreate(std::move(input)) == nullptr);
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
    constexpr const char *kProtocols[] = {
        "sub_ecu_denso_sh7055_densocan",        "sub_ecu_denso_sh7058_densocan",
        "sub_ecu_denso_sh7058s_densocan",       "sub_ecu_denso_sh7058s_diesel_densocan",
        "sub_ecu_denso_sh7059_diesel_densocan",
    };
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
    constexpr const char *kProtocols[] = {
        "sub_ecu_denso_sh7058_can",
        "sub_ecu_denso_sh7058_can_ecutek",
        "sub_ecu_denso_sh7058_can_ecutek_racerom",
        "sub_ecu_denso_sh7058_can_ecutek_racerom_alt",
        "sub_ecu_denso_sh7058_can_cobb",
    };
    for (const char *protocol : kProtocols)
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr, protocol);
    }
    for (const char *near_miss : {
             "sub_ecu_denso_sh7058_can_future",
             "sub_ecu_denso_sh7058_can_ecutek_extra",
             "sub_ecu_denso_sh7058_can_cobb_typo",
             "sub_ecu_denso_sh7058_can_diesel",
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
    constexpr std::array cases{
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
    fake->openSerialPortResult = "COM3";

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
    cancellation.cancel_on_check(2);
    NullEventSink events;
    const auto result = attempt.attempt->run(*attempt.clock, cancellation, events);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    QCOMPARE(fake->takeCallLog(),
             QStringList({"cfg:set_is_iso15765_connection:1", "cfg:set_is_can_connection:0",
                          "cfg:set_is_iso14230_connection:0", "cfg:set_is_29_bit_id:0", "cfg:set_can_speed:500000",
                          "cfg:set_can_source_address:2016", "cfg:set_can_destination_address:2024",
                          "cfg:set_iso15765_source_address:2016", "cfg:set_iso15765_destination_address:2024",
                          "open_serial_port"}));
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
        QVERIFY(fake->takeCallLog().isEmpty());
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
    fake->openSerialPortResult = "COM3";

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
    QCOMPARE(fake->takeCallLog(),
             QStringList({"cfg:set_is_iso15765_connection:1", "cfg:set_is_can_connection:0",
                          "cfg:set_is_iso14230_connection:0", "cfg:set_is_29_bit_id:0", "cfg:set_can_speed:500000",
                          "cfg:set_can_source_address:2017", "cfg:set_can_destination_address:2025",
                          "cfg:set_iso15765_source_address:2017", "cfg:set_iso15765_destination_address:2025",
                          "open_serial_port"}));
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

} // namespace
} // namespace fastecu::flash

QTEST_MAIN(fastecu::flash::FlashWorkflowTest)
#include "flash_workflow_test.moc"
