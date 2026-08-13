#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "src/backend/calibration/calibration_service.h"

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

std::optional<config::ConfigPaths> catalogPaths(const QTemporaryDir& directory,
                                                bool include_kernel_files = true)
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
    if (!QDir().mkpath(kernel_directory) ||
        !writeFile(directory.filePath("protocols.cfg"), catalog))
    {
        return std::nullopt;
    }
    if (include_kernel_files)
    {
        if (!writeFile(kernel_directory + "/catalog_mc68.bin", QByteArray::fromHex("112233")) ||
            !writeFile(kernel_directory + "/catalog_tpu.bin", QByteArray::fromHex("445566")) ||
            !writeFile(kernel_directory + "/catalog_sh7055.bin", QByteArray::fromHex("aabbccdd")))
        {
            return std::nullopt;
        }
    }
    config::ConfigPaths paths;
    paths.protocols_file = directory.filePath("protocols.cfg").toStdString();
    paths.kernel_files_directory = (kernel_directory + "/").toStdString();
    return paths;
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
    const char *portable[] = {
        "mitsu_ecu_m32r_can", "mitsu_ecu_m32r_can_vendor_ext",
        "mitsu_ecu_m32r_can_512kb", "mitsu_ecu_m32r_can_vendor_ext_512kb",
        "sub_ecu_mitsu_m32r_kline",
        "sub_ecu_hitachi_m32r_kline", "sub_ecu_hitachi_m32r_kline_recovery",
        "sub_ecu_eeprom_denso_sh7055_kline", "sub_ecu_eeprom_denso_sh7058_kline",
        "sub_ecu_eeprom_denso_sh7055_densocan", "sub_ecu_eeprom_denso_sh7058_densocan",
        "sub_ecu_eeprom_denso_sh7058_can", "sub_ecu_eeprom_denso_sh7058_can_diesel"};
    for (const char *protocol : portable)
    {
        QVERIFY2(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr, protocol);
    }
    QVERIFY(FlashWorkflowFactory::tryCreate(request("sub_ecu_hitachi_m32r_can")) == nullptr);
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
    workflow->submit(FlashAttemptResult{.success = true,
                                        .read_bytes = bytes::Bytes{0xff, 0x12},
                                        .rom_id = "123456789A_"});
    auto done = workflow->next();
    QVERIFY(std::holds_alternative<FlashCompletedStep>(done));
    QCOMPARE(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
}

void FlashWorkflowTest::subaruHitachiRoutesBothModesAndPropagatesReadResult()
{
    for (const char *protocol : {"sub_ecu_hitachi_m32r_kline",
                                 "sub_ecu_hitachi_m32r_kline_recovery"})
    {
        auto input = request(protocol);
        input.mcu = "M32R_512KB_1block";
        auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
        QVERIFY(workflow != nullptr);
        QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
        workflow->submit(FlashPromptResponse::Accept);
        QVERIFY(std::holds_alternative<FlashAttempt>(workflow->next()));
        workflow->submit(FlashAttemptResult{.success = true,
                                            .read_bytes = bytes::Bytes{0x5a},
                                            .rom_id = "123456789A_"});
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
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind,
             FlashPromptKind::ColtEraseTrigger);
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
    const auto& plan = std::get<FlashAttempt>(step).plan;
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->load_address, 0x20000u);
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
    const auto& plan = std::get<FlashAttempt>(step).plan;
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->load_address, 0xFFFF6004u);
    QCOMPARE(plan.kernel()->bytes, bytes::Bytes({0xaa, 0xbb, 0xcc, 0xdd}));

    workflow->submit(FlashAttemptResult{.success = true,
                                        .read_bytes = bytes::Bytes{0x5a},
                                        .rom_id = "123456789A_"});
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
    const auto& plan = std::get<FlashAttempt>(step).plan;
    QCOMPARE(plan.target_id(), std::string_view("sub_ecu_denso_sh7055_02_ecutek"));
    QVERIFY(plan.kernel().has_value());
    QCOMPARE(plan.kernel()->load_address, 0xFFFF6004u);
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
    QCOMPARE(std::get<FlashAttempt>(step).plan.image(), packed_image);
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
    const auto& packed = std::get<FlashAttempt>(step).plan.image();
    QVERIFY(packed.has_value());
    QCOMPARE(packed->size(), std::size_t{0x28000});
    QVERIFY(std::all_of(packed->begin(), packed->begin() + 0x20000,
                        [](bytes::Byte value)
                        { return value == 0x11; }));
    QVERIFY(std::all_of(packed->begin() + 0x20000, packed->end(),
                        [](bytes::Byte value)
                        { return value == 0x22; }));
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
    input.image = calibration::apply_flash_method_padding(
        packed_image, "sub_ecu_denso_mc68hc16y5_02");
    QCOMPARE(input.image->size(), std::size_t{0x30000});

    auto workflow = FlashWorkflowFactory::tryCreate(std::move(input));
    QVERIFY(workflow != nullptr);
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind, FlashPromptKind::Begin);
    workflow->submit(FlashPromptResponse::Accept);
    auto step = workflow->next();
    QVERIFY(std::holds_alternative<FlashAttempt>(step));
    QCOMPARE(std::get<FlashAttempt>(step).plan.image(), packed_image);
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
    QCOMPARE(std::get<FlashPromptStep>(workflow->next()).kind,
             FlashPromptKind::CycleIgnition);
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
    const auto& kernel = std::get<FlashAttempt>(step).plan.kernel();
    QVERIFY(kernel.has_value());
    QCOMPARE(kernel->load_address, 0x20000u);
    QCOMPARE(kernel->bytes, bytes::Bytes({0x44, 0x55, 0x66}));
}

} // namespace
} // namespace fastecu::flash

QTEST_MAIN(fastecu::flash::FlashWorkflowTest)
#include "flash_workflow_test.moc"
