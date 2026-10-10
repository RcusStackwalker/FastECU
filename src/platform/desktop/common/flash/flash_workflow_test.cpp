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
#include <tuple>
#include <utility>
#include <vector>

#include "src/algorithms/memory/memory_image.h"
#include "src/backend/config/builtin_catalog.h"
#include "src/backend/config/catalog.h"
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

// The kernel each protocol declared in the synthetic catalog these tests
// used to write as protocols.cfg; catalogPaths() writes the files.
struct CatalogKernel
{
    std::string_view protocol;
    std::string_view file;
    std::optional<std::uint32_t> load_address;
};

constexpr auto kCatalogKernels = std::to_array<CatalogKernel>({
    {"sub_ecu_denso_mc68hc16y5_02", "catalog_mc68.bin", 0x20000U},
    {"sub_ecu_denso_mc68hc16y5_02_tpu", "catalog_tpu.bin", 0x20000U},
    {"sub_ecu_denso_mc68hc16y5_02_bdm", "catalog_mc68.bin", 0x20000U},
    {"sub_ecu_denso_sh7055_02", "catalog_sh7055.bin", 0xFFFF6004U},
    {"sub_ecu_denso_sh7055_02_ecutek", "catalog_sh7055.bin", 0xFFFF6004U},
    {"sub_ecu_denso_sh7055_densocan", "catalog_densocan.bin", 0xFFFF6004U},
    {"sub_tcu_denso_sh7055_can", "catalog_tcu_sh7055.bin", 0xFFFF9000U},
    {"sub_tcu_denso_sh7058_can", "catalog_tcu_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_ecutek", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_cobb", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_diesel", "catalog_diesel_sh7058.bin", 0xFFFF4000U},
    {"sub_ecu_denso_sh7059_can_diesel", "catalog_diesel_sh7059.bin", 0xFFFEE000U},
    {"sub_ecu_denso_sh7055_04", "catalog_kline_sh7055.bin", 0xFFFF6004U},
    {"sub_ecu_denso_sh7058_ecutek", "catalog_kline_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_cobb", "catalog_kline_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_unisia_jecs_20_bootmode", "catalog_uj20_bootmode.bin", std::nullopt},
    {"sub_ecu_unisia_jecs_30_bootmode", "catalog_uj30_bootmode.bin", std::nullopt},
});

// `protocol` must outlive the request: pass a literal.
FlashWorkflowRequest Request(std::string_view protocol, FlashOperation operation = FlashOperation::kRead)
{
    config::ProtocolSpec spec{.name = protocol, .mcu = "M32R_384KB_1block"};
    if (const auto kernel = std::ranges::find(kCatalogKernels, protocol, &CatalogKernel::protocol);
        kernel != kCatalogKernels.end())
    {
        spec.kernel = kernel->file;
        spec.kernel_load_address = kernel->load_address;
    }
    return {.operation = operation,
            .protocol = spec,
            .image = std::nullopt,
            .paths = {},
            .display_filename = "test.bin",
            .serial = nullptr};
}

// An MC68HC16Y5 `_02` K-Line request carrying the built-in catalog's memory
// maps, as the application's selected protocol does.
FlashWorkflowRequest Mc68Request(FlashOperation operation)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02", operation);
    input.protocol.mcu = "MC68HC16Y5";
    input.protocol.memory_maps = config::BuiltinCatalog().FindProtocol("sub_ecu_denso_mc68hc16y5_02")->memory_maps;
    return input;
}

// The bytes `plan`'s ROM image holds at ECU addresses [start, start + size).
bytes::Bytes RenderedFlash(const FlashPlan& plan, std::uint32_t start, std::uint32_t size)
{
    const std::optional<memory::MemoryImage>& image = plan.RomImage();
    if (!image.has_value())
    {
        return {};
    }
    const auto range =
        memory::AddressRange<memory::FlashSpace>::Make(memory::FlashAddress{start}, memory::ByteCount{size});
    const auto view = range.has_value() ? image->Render(*range) : std::unexpected(memory::MemoryError{});
    return view.has_value() ? bytes::Bytes(view->Data().begin(), view->Data().end()) : bytes::Bytes{};
}

FlashWorkflowRequest UnisiaM32rWrite()
{
    auto input = Request("sub_ecu_unisia_jecs_20", FlashOperation::kWrite);
    input.protocol.mcu = "M32R_128KB";
    input.image = bytes::Bytes(0x20000, 0xff);
    return input;
}

// Begin -> ApplyProgrammingVoltage -> attempt, all accepted.
std::unique_ptr<FlashWorkflow> UnisiaM32rWriteAtAttempt()
{
    auto workflow = FlashWorkflowFactory::TryCreate(UnisiaM32rWrite());
    if (workflow == nullptr || std::get<FlashPromptStep>(workflow->Next()).kind != FlashPromptKind::kBegin)
    {
        return nullptr;
    }
    workflow->Submit(FlashPromptResponse::kAccept);
    if (std::get<FlashPromptStep>(workflow->Next()).kind != FlashPromptKind::kApplyProgrammingVoltage)
    {
        return nullptr;
    }
    workflow->Submit(FlashPromptResponse::kAccept);
    if (!std::holds_alternative<FlashAttempt>(workflow->Next()))
    {
        return nullptr;
    }
    return workflow;
}

using PromptArguments = std::vector<std::pair<std::string, std::string>>;

FlashWorkflowRequest UnisiaBootmodeWrite(const config::ConfigPaths& paths)
{
    auto input = Request("sub_ecu_unisia_jecs_20_bootmode", FlashOperation::kWrite);
    input.protocol.mcu = "M32R_128KB";
    input.image = bytes::Bytes(0x20000, 0xa5);
    input.paths = paths;
    return input;
}

// Begin -> ApplyBootModeVoltages -> kernel attempt, all accepted.
std::unique_ptr<FlashWorkflow> UnisiaBootmodeAtKernelAttempt(const config::ConfigPaths& paths)
{
    auto workflow = FlashWorkflowFactory::TryCreate(UnisiaBootmodeWrite(paths));
    if (workflow == nullptr || std::get<FlashPromptStep>(workflow->Next()).kind != FlashPromptKind::kBegin)
    {
        return nullptr;
    }
    workflow->Submit(FlashPromptResponse::kAccept);
    if (std::get<FlashPromptStep>(workflow->Next()).kind != FlashPromptKind::kApplyBootModeVoltages)
    {
        return nullptr;
    }
    workflow->Submit(FlashPromptResponse::kAccept);
    if (!std::holds_alternative<FlashAttempt>(workflow->Next()))
    {
        return nullptr;
    }
    return workflow;
}

PromptArguments BootmodeNotice(std::string outcome)
{
    return {{"outcome", std::move(outcome)}, {"external_vpp", "yes"}, {"power_off_advice", "no"}};
}

bool WriteFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

std::optional<config::ConfigPaths> CatalogPaths(const QTemporaryDir& directory, bool include_kernel_files = true)
{
    const QString kernel_directory = directory.filePath("kernels");
    if (!QDir().mkpath(kernel_directory))
    {
        return std::nullopt;
    }
    if (include_kernel_files)
    {
        if (!WriteFile(kernel_directory + "/catalog_mc68.bin", QByteArray::fromHex("112233")) ||
            !WriteFile(kernel_directory + "/catalog_tpu.bin", QByteArray::fromHex("445566")) ||
            !WriteFile(kernel_directory + "/catalog_sh7055.bin", QByteArray::fromHex("aabbccdd")) ||
            !WriteFile(kernel_directory + "/catalog_densocan.bin", QByteArray::fromHex("aabbccdd")) ||
            !WriteFile(kernel_directory + "/catalog_tcu_sh7055.bin", QByteArray::fromHex("10203040")) ||
            !WriteFile(kernel_directory + "/catalog_tcu_sh7058.bin", QByteArray::fromHex("50607080")) ||
            !WriteFile(kernel_directory + "/catalog_petrol_sh7058.bin", QByteArray::fromHex("90a0b0c0")) ||
            !WriteFile(kernel_directory + "/catalog_diesel_sh7058.bin", QByteArray::fromHex("d0e0f001")) ||
            !WriteFile(kernel_directory + "/catalog_diesel_sh7059.bin", QByteArray::fromHex("d0e0f002")) ||
            !WriteFile(kernel_directory + "/catalog_kline_sh7055.bin", QByteArray::fromHex("aabbccdd")) ||
            !WriteFile(kernel_directory + "/catalog_kline_sh7058.bin", QByteArray::fromHex("01020304")) ||
            !WriteFile(kernel_directory + "/catalog_uj20_bootmode.bin", QByteArray::fromHex("0102030405")) ||
            !WriteFile(kernel_directory + "/catalog_uj30_bootmode.bin", QByteArray::fromHex("0607")))
        {
            return std::nullopt;
        }
    }
    config::ConfigPaths paths;
    paths.kernel_files_directory = (kernel_directory + "/").toStdString();
    return paths;
}

std::unique_ptr<SerialPortActions> RecordingSerial(FakeBackend **fake)
{
    auto serial = std::make_unique<SerialPortActions>(
        [fake]() -> SerialBackend *
        {
            *fake = new NiceFakeBackend;
            return *fake;
        });
    if (!serial->SetAddSsmHeader(false) || *fake == nullptr)
    {
        return nullptr;
    }
    return serial;
}

void ExpectCanTransportSetup(FakeBackend& fake, bool reset, std::uint32_t source, std::uint32_t destination)
{
    ::testing::InSequence sequence;
    if (reset)
    {
        EXPECT_CALL(fake, ResetConnection()).WillOnce(::testing::Return());
    }
    EXPECT_CALL(fake, SetIsIso15765Connection(true)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, SetIsCanConnection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, SetIsIso14230Connection(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, SetIs29BitId(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, SetCanSpeed(QStringLiteral("500000"))).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, SetCanSourceAddress(source)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, SetCanDestinationAddress(destination)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, SetIso15765SourceAddress(source)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, SetIso15765DestinationAddress(destination)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, SetAddIso14230Header(false)).WillOnce(::testing::Return(true));
    EXPECT_CALL(fake, OpenSerialPort()).WillOnce(::testing::Return(QStringLiteral("COM3")));
}

void ExpectNoBackendIo(FakeBackend& fake)
{
    EXPECT_CALL(fake, IsSerialPortOpen()).Times(0);
    EXPECT_CALL(fake, ResetConnection()).Times(0);
    EXPECT_CALL(fake, ChangePortSpeed(::testing::_)).Times(0);
    EXPECT_CALL(fake, OpenSerialPort()).Times(0);
    EXPECT_CALL(fake, ReadSerialData(::testing::_)).Times(0);
    EXPECT_CALL(fake, WriteSerialData(::testing::_)).Times(0);
    EXPECT_CALL(fake, WriteSerialDataEchoCheck(::testing::_)).Times(0);
    EXPECT_CALL(fake, ReadVbatt()).Times(0);
}

TEST(FlashWorkflowTest, recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone)
{
    static constexpr auto kPortable = std::to_array<const char *>({"mitsu_ecu_m32r_can",
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
    for (const char *protocol : kPortable)
    {
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(protocol)) != nullptr) << protocol;
    }
}

TEST(FlashWorkflowTest, invalidColtSuffixIsRecognizedButFailsPreflight)
{
    auto workflow = FlashWorkflowFactory::TryCreate(Request("mitsu_ecu_m32r_can_typo"));
    ASSERT_TRUE(workflow != nullptr);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
}

TEST(FlashWorkflowTest, preflightPrecedesPromptsAndDeclineCancels)
{
    auto invalid = Request("mitsu_ecu_m32r_can", FlashOperation::kTestWrite);
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(invalid));
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(workflow->Next()));

    workflow = FlashWorkflowFactory::TryCreate(Request("mitsu_ecu_m32r_can"));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kDecline);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, successfulReadBytesAreAcceptedAutomatically)
{
    auto workflow = FlashWorkflowFactory::TryCreate(Request("mitsu_ecu_m32r_can"));
    std::ignore = workflow->Next();
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    workflow->Submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1, 2, 3}});
    auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({1, 2, 3}));
}

TEST(FlashWorkflowTest, unisiaJecsRoutesOnlyExactProtocolMcuPairs)
{
    static constexpr auto kPairs = std::to_array<std::pair<const char *, const char *>>({
        {"sub_ecu_unisia_jecs_m3779x", "M3779x"},
        {"sub_ecu_unisia_jecs_m3775x", "M3775x"},
    });

    for (const auto& [protocol, mcu] : kPairs)
    {
        auto input = Request(protocol);
        input.protocol.mcu = mcu;
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << protocol;
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruUnisiaJecs);
        ASSERT_EQ(plan.Transport(), TransportKind::kKline);
        ASSERT_EQ(plan.TargetId(), protocol);
        ASSERT_EQ(plan.McuName(), mcu);
    }

    ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request("sub_ecu_unisia_jecs_m3779x_suffix")) == nullptr);
    ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request("sub_ecu_unisia_jecs_m3775x_suffix")) == nullptr);
}

TEST(FlashWorkflowTest, unisiaJecsCrossPairsFailBeforeAttempt)
{
    static constexpr auto kCrossPairs = std::to_array<std::pair<const char *, const char *>>({
        {"sub_ecu_unisia_jecs_m3779x", "M3775x"},
        {"sub_ecu_unisia_jecs_m3775x", "M3779x"},
    });

    for (const auto& [protocol, mcu] : kCrossPairs)
    {
        auto input = Request(protocol);
        input.protocol.mcu = mcu;
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << protocol;
        auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
    }
}

TEST(FlashWorkflowTest, subaruMitsuPropagatesRomId)
{
    auto input = Request("sub_ecu_mitsu_m32r_kline");
    input.protocol.mcu = "M32R_512KB_4blocks";
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    workflow->Submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0xff, 0x12}, .rom_id = "123456789A_"});
    auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
}

TEST(FlashWorkflowTest, subaruHitachiRoutesBothModesAndPropagatesReadResult)
{
    for (const char *protocol : {"sub_ecu_hitachi_m32r_kline", "sub_ecu_hitachi_m32r_kline_recovery"})
    {
        auto input = Request(protocol);
        input.protocol.mcu = "M32R_512KB_1block";
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
        workflow->Submit(
            FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
        auto done = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
        ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5a}));
        ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
    }
}

TEST(FlashWorkflowTest, routesTcuHitachiM32rKlineReadOnly)
{
    auto input = Request("sub_tcu_hitachi_m32r_kline");
    input.protocol.mcu = "M32R_512KB";
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    workflow->Submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5a}));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));

    // Write is rejected by the plan builder (the family is read-only), so the
    // workflow's very first step must be a failure rather than a prompt or an
    // attempt -- the legacy path silently "succeeded" while writing nothing.
    auto write_request = Request("sub_tcu_hitachi_m32r_kline");
    write_request.protocol.mcu = "M32R_512KB";
    write_request.operation = FlashOperation::kWrite;
    write_request.image = bytes::Bytes(0x80000, 0x00);
    auto write_workflow = FlashWorkflowFactory::TryCreate(std::move(write_request));
    ASSERT_TRUE(write_workflow != nullptr);
    const auto write_step = write_workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(write_step));
    ASSERT_EQ(std::get<FlashFailureStep>(write_step).error.kind, ErrorKind::kUnsupported);
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
    auto read_input = Request(kProtocol);
    read_input.protocol.mcu = kMcu;
    auto read_workflow = FlashWorkflowFactory::TryCreate(std::move(read_input));
    ASSERT_TRUE(read_workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(read_workflow->Next()).kind, FlashPromptKind::kBegin);
    read_workflow->Submit(FlashPromptResponse::kAccept);
    const auto read_step = read_workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(read_step));
    const FlashPlan& read_plan = std::get<FlashAttempt>(read_step).attempt->Plan();
    ASSERT_EQ(read_plan.TargetId(), std::string_view(kProtocol));
    ASSERT_TRUE(read_plan.Operation() == FlashOperation::kRead);
    ASSERT_EQ(read_plan.Transport(), TransportKind::kCanIso15765);
    read_workflow->Submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}});
    const auto read_done = read_workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(read_done));
    ASSERT_EQ(std::get<FlashCompletedStep>(read_done).outcome, FlashWorkflowOutcome::kSucceeded);
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
    auto test_write_input = Request(kProtocol, FlashOperation::kTestWrite);
    test_write_input.protocol.mcu = kMcu;
    test_write_input.image = bytes::Bytes(0x80000, 0xa5);
    auto test_write_workflow = FlashWorkflowFactory::TryCreate(std::move(test_write_input));
    ASSERT_TRUE(test_write_workflow != nullptr);
    const auto test_write_step = test_write_workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(test_write_step));
    ASSERT_EQ(std::get<FlashFailureStep>(test_write_step).error.kind, ErrorKind::kUnsupported);

    // Write, unlike the K-Line sibling, is supported by this family and
    // routes all the way to an attempt bound to the CAN executor/transport.
    auto write_input = Request(kProtocol, FlashOperation::kWrite);
    write_input.protocol.mcu = kMcu;
    write_input.image = bytes::Bytes(0x80000, 0xa5);
    auto write_workflow = FlashWorkflowFactory::TryCreate(std::move(write_input));
    ASSERT_TRUE(write_workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(write_workflow->Next()).kind, FlashPromptKind::kBegin);
    write_workflow->Submit(FlashPromptResponse::kAccept);
    const auto write_step = write_workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(write_step));
    const FlashPlan& write_plan = std::get<FlashAttempt>(write_step).attempt->Plan();
    ASSERT_EQ(write_plan.TargetId(), std::string_view(kProtocol));
    ASSERT_TRUE(write_plan.Operation() == FlashOperation::kWrite);
    ASSERT_EQ(write_plan.Transport(), TransportKind::kCanIso15765);
}

TEST(FlashWorkflowTest, routesSh72543rAliasesAndPreservesImageAndIdentity)
{
    for (const char *protocol : {"sub_ecu_hitachi_sh72543r_can", "sub_ecu_hitachi_sh72543r_can_recovery"})
    {
        for (auto operation : {FlashOperation::kRead, FlashOperation::kWrite})
        {
            auto input = Request(protocol, operation);
            input.protocol.mcu = "SH72543R";
            if (operation == FlashOperation::kWrite)
            {
                input.image = bytes::Bytes(0x200000, 0xa5);
            }
            auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
            ASSERT_TRUE(workflow);
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
            workflow->Submit(FlashPromptResponse::kAccept);
            auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
            const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
            ASSERT_EQ(plan.Family(), FlashFamily::kSubaruHitachiSh72543rCan);
            ASSERT_EQ(plan.TargetId(), std::string_view(protocol));
            ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
            ASSERT_EQ(plan.TransferRegion().start, operation == FlashOperation::kRead ? 0U : 0x6000U);
            if (operation == FlashOperation::kWrite)
            {
                ASSERT_EQ(plan.Image(), bytes::Bytes(0x200000, 0xa5));
            }
            workflow->Submit(FlashAttemptResult{
                .success = true,
                .read_bytes = operation == FlashOperation::kRead ? std::optional{bytes::Bytes{1, 2, 3}} : std::nullopt,
                .rom_id =
                    operation == FlashOperation::kRead ? std::optional<std::string>{"CAL_1122334455_"} : std::nullopt});
            auto done = std::get<FlashCompletedStep>(workflow->Next());
            ASSERT_EQ(done.outcome, FlashWorkflowOutcome::kSucceeded);
            if (operation == FlashOperation::kRead)
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
    ASSERT_TRUE(!FlashWorkflowFactory::TryCreate(Request("sub_ecu_hitachi_sh72543r_can_recovery_typo")));
    ASSERT_TRUE(!FlashWorkflowFactory::TryCreate(Request("sub_ecu_hitachi_sh72543r_can_typo")));
}
TEST(FlashWorkflowTest, routesSh7058ReadAndWriteWithPreTransportPrompts)
{
    ASSERT_TRUE(!FlashWorkflowFactory::TryCreate(Request("sub_ecu_hitachi_sh7058_can_extra")));
    for (const auto operation : {FlashOperation::kRead, FlashOperation::kWrite})
    {
        auto input = Request("sub_ecu_hitachi_sh7058_can", operation);
        input.protocol.mcu = "SH7058_1block";
        if (operation == FlashOperation::kWrite)
        {
            input.image = bytes::Bytes(0x100000, 0x5a);
        }
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow);
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        if (operation == FlashOperation::kRead)
        {
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kConfirmSh7058Read);
            workflow->Submit(FlashPromptResponse::kAccept);
        }
        auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruHitachiSh7058);
        ASSERT_EQ(plan.Transport(),
                  operation == FlashOperation::kRead ? TransportKind::kKline : TransportKind::kCanIso15765);
    }
    auto input = Request("sub_ecu_hitachi_sh7058_can");
    input.protocol.mcu = "SH7058_1block";
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    workflow->Submit(FlashPromptResponse::kAccept);
    workflow->Submit(FlashPromptResponse::kDecline);
    ASSERT_EQ(std::get<FlashCompletedStep>(workflow->Next()).outcome, FlashWorkflowOutcome::kCancelled);
}
TEST(FlashWorkflowTest, sh72543rRejectsPreflightAndDeclinedBegin)
{
    for (const char *protocol : {"sub_ecu_hitachi_sh72543r_can", "sub_ecu_hitachi_sh72543r_can_recovery"})
    {
        for (int fault = 0; fault < 3; ++fault)
        {
            auto input = Request(protocol, fault == 0 ? FlashOperation::kTestWrite : FlashOperation::kWrite);
            input.protocol.mcu = fault == 1 ? "SH72543d" : "SH72543R";
            input.image = bytes::Bytes(fault == 2 ? 16 : 0x200000);
            auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
            ASSERT_TRUE(workflow);
            auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind,
                      fault == 0 ? ErrorKind::kUnsupported : ErrorKind::kInvalidConfig);
        }
        auto input = Request(protocol);
        input.protocol.mcu = "SH72543R";
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow);
        ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(workflow->Next()));
        workflow->Submit(FlashPromptResponse::kDecline);
        auto done = std::get<FlashCompletedStep>(workflow->Next());
        ASSERT_EQ(done.outcome, FlashWorkflowOutcome::kCancelled);
        ASSERT_TRUE(!done.accepted_read_bytes);
    }
}
TEST(FlashWorkflowTest, sh72543rPropagatesFailureAndAbsentIdentity)
{
    for (int outcome = 0; outcome < 3; ++outcome)
    {
        auto input = Request("sub_ecu_hitachi_sh72543r_can");
        input.protocol.mcu = "SH72543R";
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow);
        workflow->Submit(FlashPromptResponse::kAccept);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
        workflow->Submit(
            FlashAttemptResult{.success = outcome == 0,
                               .error_kind = outcome == 1 ? ErrorKind::kDisconnected : ErrorKind::kCancelled,
                               .error_detail = "lost adapter",
                               .read_bytes = outcome == 0 ? std::optional{bytes::Bytes{4, 5}} : std::nullopt});
        auto step = workflow->Next();
        if (outcome == 1)
        {
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kDisconnected);
        }
        else
        {
            auto done = std::get<FlashCompletedStep>(step);
            ASSERT_TRUE(!done.rom_id);
            ASSERT_EQ(done.outcome, outcome == 0 ? FlashWorkflowOutcome::kSucceeded : FlashWorkflowOutcome::kCancelled);
        }
    }
}

TEST(FlashWorkflowTest, coltWriteUsesColtSpecificSafetyPrompts)
{
    auto write = Request("mitsu_ecu_m32r_can", FlashOperation::kWrite);
    write.image = bytes::Bytes(0x60000);
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(write));

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kColtEraseTrigger);
}

TEST(FlashWorkflowTest, mc68BdmReadRoutesThroughBeginToAttempt)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm");
    input.protocol.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoMc68hc16y502Bdm);
    ASSERT_EQ(plan.Transport(), TransportKind::kKline);
    ASSERT_EQ(plan.TransferRegion(), (MemoryRegion{0, 0x30000}));
    ASSERT_TRUE(!plan.Image().has_value());
}

TEST(FlashWorkflowTest, mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::kWrite);
    input.protocol.mcu = "MC68HC16Y5";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    input.image = bytes::Bytes(0x30000, 0x5a);
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        FAIL() << failure->error.detail.c_str();
    }
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kConfirmBdmKernelBootstrap);
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
    bytes::Bytes expected(0x20, 0x00);
    expected[0] = 0x11;
    expected[1] = 0x22;
    expected[2] = 0x33;
    ASSERT_EQ(plan.Image(), std::optional<bytes::Bytes>(expected));
    ASSERT_EQ(plan.TransferRegion(), (MemoryRegion{0x20000, 0x20}));
    ASSERT_TRUE(!plan.Kernel().has_value());
}

TEST(FlashWorkflowTest, mc68BdmDeclinedBootstrapConfirmationCancels)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::kWrite);
    input.protocol.mcu = "MC68HC16Y5";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kConfirmBdmKernelBootstrap);
    workflow->Submit(FlashPromptResponse::kDecline);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, mc68BdmDeclinedBeginCancels)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm");
    input.protocol.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kDecline);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, mc68BdmTestWriteFailsBeforeAnyPrompt)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::kTestWrite);
    input.protocol.mcu = "MC68HC16Y5";
    input.image = bytes::Bytes(0x30000, 0x5a);
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kUnsupported);
}

TEST(FlashWorkflowTest, mc68BdmPrefixLookalikeStaysOffTheKlineFamily)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm_x");
    input.protocol.mcu = "MC68HC16Y5";
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
}

TEST(FlashWorkflowTest, mc68TpuProtocolIsClaimedByPortableRoute)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_tpu");
    input.protocol.mcu = "MC68HC16Y5_TPU";
    ASSERT_TRUE(FlashWorkflowFactory::TryCreate(std::move(input)) != nullptr);
}

TEST(FlashWorkflowTest, mc68Revision04HasNoRoute)
{
    for (const char *protocol : {"sub_ecu_denso_mc68hc16y5_04", "sub_ecu_denso_mc68hc16y5_04_ecutek"})
    {
        auto input = Request(protocol);
        input.protocol.mcu = "MC68HC16Y5";
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(std::move(input)) == nullptr) << protocol;
    }
}

TEST(FlashWorkflowTest, sh7055ProtocolIsClaimedByPortableRoute)
{
    auto input = Request("sub_ecu_denso_sh7055_02");
    input.protocol.mcu = "SH7055";
    ASSERT_TRUE(FlashWorkflowFactory::TryCreate(std::move(input)) != nullptr);
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
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(protocol)) != nullptr) << protocol;
    }
    for (const char *near_miss : {"sub_ecu_denso_sh7058_densocan_extra", "future_densocan"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(near_miss)) == nullptr) << near_miss;
    }
}

TEST(FlashWorkflowTest, densoCanResolvesKernelPromptsAndPropagatesAttemptResult)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Request("sub_ecu_denso_sh7055_densocan");
    input.protocol.mcu = "SH7055";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_densocan.bin")));
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kCycleIgnition);
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_EQ(plan.Transport(), TransportKind::kCanRawIso15765);
    ASSERT_TRUE(plan.Kernel().has_value());
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0xaa, 0xbb, 0xcc, 0xdd}));

    workflow->Submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
    const auto& done = std::get<FlashCompletedStep>(step);
    ASSERT_EQ(done.outcome, FlashWorkflowOutcome::kSucceeded);
    ASSERT_EQ(done.accepted_read_bytes, bytes::Bytes({0x5a}));
    ASSERT_EQ(done.rom_id, std::string("123456789A_"));
}

TEST(FlashWorkflowTest, densoCanMissingKernelAddressFailsBeforeAnyPromptOrAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto input = Request("sub_ecu_denso_sh7055_densocan");
    input.protocol.mcu = "SH7055";
    input.paths = *paths;
    input.protocol.kernel_load_address.reset();

    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_NE(workflow, nullptr);
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    const auto& error = std::get<FlashFailureStep>(step).error;
    EXPECT_EQ(error.kind, ErrorKind::kInvalidConfig);
    EXPECT_THAT(error.detail, ::testing::HasSubstr("declares no kernel load address"));
}

TEST(FlashWorkflowTest, densoCanPreflightAndDeclinedPromptsStopBeforeAttempt)
{
    auto missing_kernel = Request("sub_ecu_denso_sh7055_densocan");
    missing_kernel.protocol.mcu = "SH7055";
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(missing_kernel));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(workflow->Next()));

    for (const bool decline_begin : {true, false})
    {
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        auto input = Request("sub_ecu_denso_sh7055_densocan");
        input.protocol.mcu = "SH7055";
        const auto paths = CatalogPaths(directory);
        ASSERT_TRUE(paths.has_value());
        input.paths = *paths;
        workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        if (decline_begin)
        {
            workflow->Submit(FlashPromptResponse::kDecline);
        }
        else
        {
            workflow->Submit(FlashPromptResponse::kAccept);
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kCycleIgnition);
            workflow->Submit(FlashPromptResponse::kDecline);
        }
        const auto done = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
        ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
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
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(protocol)) != nullptr) << protocol;
    }
    for (const char *near_miss : {
             "sub_ecu_denso_sh7058_can_future",
             "sub_ecu_denso_sh7058_can_ecutek_extra",
             "sub_ecu_denso_sh7058_can_cobb_typo",
         })
    {
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(near_miss)) == nullptr) << near_miss;
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
        Case{"sub_ecu_denso_sh7058_can", SubaruDensoSh7058CanSecurity::kStock, FlashOperation::kRead},
        Case{"sub_ecu_denso_sh7058_can_ecutek", SubaruDensoSh7058CanSecurity::kEcuTek, FlashOperation::kTestWrite},
        Case{"sub_ecu_denso_sh7058_can_ecutek_racerom", SubaruDensoSh7058CanSecurity::kRaceRom, FlashOperation::kWrite},
        Case{"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", SubaruDensoSh7058CanSecurity::kRaceRomAlt,
             FlashOperation::kRead},
        Case{"sub_ecu_denso_sh7058_can_cobb", SubaruDensoSh7058CanSecurity::kCobb, FlashOperation::kTestWrite},
    };

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const Case& test : cases)
    {
        auto input = Request(test.protocol, test.operation);
        input.protocol.mcu = "SH7058";
        input.paths = *paths;
        if (test.operation != FlashOperation::kRead)
        {
            input.image = bytes::Bytes(0x00100000, bytes::Byte{0xA5});
        }
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << test.protocol;

        auto step = workflow->Next();
        if (const auto *failure = std::get_if<FlashFailureStep>(&step))
        {
            FAIL() << failure->error.detail.c_str();
        }
        ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
        ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoSh7058Can);
        ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
        ASSERT_EQ(plan.TargetId(), std::string_view(test.protocol));
        ASSERT_EQ(plan.McuName(), std::string_view("SH7058"));
        ASSERT_EQ(plan.Operation(), test.operation);
        ASSERT_TRUE(plan.Confirmations().empty());
        ASSERT_TRUE(plan.Kernel().has_value());
        ASSERT_EQ(plan.Kernel()->load_address, 0xFFFF3000U);
        ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0x90, 0xA0, 0xB0, 0xC0}));
        const auto *family_plan = std::get_if<SubaruDensoSh7058CanPlan>(&plan.FamilyPlan());
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
    auto input = Request("sub_ecu_denso_sh7058_can");
    input.protocol.mcu = "SH7058";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    workflow->Submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5A, 0xA5}, .rom_id = "CALID_123456789A_"});
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kSucceeded);
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5A, 0xA5}));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("CALID_123456789A_"));
}

TEST(FlashWorkflowTest, petrolReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ExpectCanTransportSetup(*fake, true, 2016, 2024);

    auto input = Request("sub_ecu_denso_sh7058_can");
    input.protocol.mcu = "SH7058";
    input.paths = *paths;
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    // Resolution happened before Begin; removing the kernel now must not
    // affect the already-bound attempt.
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_petrol_sh7058.bin")));

    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->Plan();
    ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoSh7058Can);
    ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
    ASSERT_EQ(plan.TargetId(), std::string_view("sub_ecu_denso_sh7058_can"));
    ASSERT_TRUE(plan.Kernel().has_value());
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0x90, 0xA0, 0xB0, 0xC0}));

    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(5);
    NullEventSink events;
    const auto result = attempt.attempt->Run(*attempt.clock, cancellation, events);
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kCancelled);
}

TEST(FlashWorkflowTest, dieselRoutesOnlyTheTwoExactProtocols)
{
    for (const char *protocol : {"sub_ecu_denso_sh7058_can_diesel", "sub_ecu_denso_sh7059_can_diesel"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(protocol)) != nullptr) << protocol;
    }
    for (const char *near_miss : {"sub_ecu_denso_sh7058_can_diesel_future", "sub_ecu_denso_sh7059_can_diesel_extra",
                                  "sub_ecu_denso_sh7058_can_diesel_typo", "sub_ecu_denso_sh7058_can_diesel_ecutek"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(near_miss)) == nullptr) << near_miss;
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
        Case{"sub_ecu_denso_sh7058_can_diesel", "SH7058d", FlashOperation::kRead, 0x00100000, 0xFFFF4000,
             QByteArray::fromHex("d0e0f001")},
        Case{"sub_ecu_denso_sh7058_can_diesel", "SH7058d", FlashOperation::kTestWrite, 0x00100000, 0xFFFF4000,
             QByteArray::fromHex("d0e0f001")},
        Case{"sub_ecu_denso_sh7058_can_diesel", "SH7058d", FlashOperation::kWrite, 0x00100000, 0xFFFF4000,
             QByteArray::fromHex("d0e0f001")},
        Case{"sub_ecu_denso_sh7059_can_diesel", "SH7059d", FlashOperation::kRead, 0x00180000, 0xFFFEE000,
             QByteArray::fromHex("d0e0f002")},
        Case{"sub_ecu_denso_sh7059_can_diesel", "SH7059d", FlashOperation::kTestWrite, 0x00180000, 0xFFFEE000,
             QByteArray::fromHex("d0e0f002")},
        Case{"sub_ecu_denso_sh7059_can_diesel", "SH7059d", FlashOperation::kWrite, 0x00180000, 0xFFFEE000,
             QByteArray::fromHex("d0e0f002")},
    };

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    for (const Case& test : cases)
    {
        auto input = Request(test.protocol, test.operation);
        input.protocol.mcu = test.mcu;
        input.paths = *paths;
        if (test.operation != FlashOperation::kRead)
        {
            input.image = bytes::Bytes(test.rom_size, bytes::Byte{0xA5});
        }
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << test.protocol;
        auto step = workflow->Next();
        if (const auto *failure = std::get_if<FlashFailureStep>(&step))
        {
            FAIL() << failure->error.detail.c_str();
        }
        ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
        ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoSh7058CanDiesel);
        ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
        ASSERT_EQ(plan.TargetId(), std::string_view(test.protocol));
        ASSERT_EQ(plan.McuName(), std::string_view(test.mcu));
        ASSERT_EQ(plan.Operation(), test.operation);
        ASSERT_EQ(plan.TransferRegion().length, static_cast<std::uint32_t>(test.rom_size));
        ASSERT_TRUE(plan.Confirmations().empty());
        ASSERT_TRUE(plan.Kernel().has_value());
        ASSERT_EQ(plan.Kernel()->load_address, test.kernel_address);
        ASSERT_EQ(QByteArray(reinterpret_cast<const char *>(plan.Kernel()->bytes.data()),
                             static_cast<int>(plan.Kernel()->bytes.size())),
                  test.kernel_bytes);
        const auto *family_plan = std::get_if<SubaruDensoSh7058CanDieselPlan>(&plan.FamilyPlan());
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
    auto input = Request("sub_ecu_denso_sh7059_can_diesel");
    input.protocol.mcu = "SH7059d";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_diesel_sh7059.bin")));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto attempt_step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(attempt_step));
    const FlashPlan& snapshot = std::get<FlashAttempt>(attempt_step).attempt->Plan();
    ASSERT_TRUE(snapshot.Kernel().has_value());
    ASSERT_EQ(snapshot.Kernel()->load_address, 0xFFFEE000U);
    ASSERT_EQ(snapshot.Kernel()->bytes, bytes::Bytes({0xD0, 0xE0, 0xF0, 0x02}));

    workflow->Submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0xD1, 0xE5}, .rom_id = "DIESEL_CAL_ECU_"});
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kSucceeded);
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0xD1, 0xE5}));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("DIESEL_CAL_ECU_"));
}

TEST(FlashWorkflowTest, dieselReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ExpectCanTransportSetup(*fake, true, 2016, 2024);

    auto input = Request("sub_ecu_denso_sh7059_can_diesel");
    input.protocol.mcu = "SH7059d";
    input.paths = *paths;
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);

    // The workflow owns the resolved kernel before Begin; this also proves
    // the Diesel route is a real DesktopCan attempt rather than a legacy
    // MainWindow branch.
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_diesel_sh7059.bin")));
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->Plan();
    ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoSh7058CanDiesel);
    ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
    ASSERT_EQ(plan.TargetId(), std::string_view("sub_ecu_denso_sh7059_can_diesel"));
    ASSERT_EQ(plan.McuName(), std::string_view("SH7059d"));
    ASSERT_TRUE(plan.Kernel().has_value());
    ASSERT_EQ(plan.Kernel()->load_address, 0xFFFEE000U);
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0xD0, 0xE0, 0xF0, 0x02}));

    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(53);
    NullEventSink events;
    const auto result = attempt.attempt->Run(*attempt.clock, cancellation, events);
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kCancelled);
}

TEST(FlashWorkflowTest, tcuRoutesOnlyTheTwoExactProtocols)
{
    for (const char *protocol : {"sub_tcu_denso_sh7055_can", "sub_tcu_denso_sh7058_can"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(protocol)) != nullptr) << protocol;
    }
    for (const char *near_miss :
         {"sub_tcu_denso_sh7055_can_future", "sub_tcu_denso_sh7058_can_typo", "sub_tcu_denso_sh7058_can_extra"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(near_miss)) == nullptr) << near_miss;
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
        Case{"sub_tcu_denso_sh7055_can", "SH7055", FlashOperation::kRead, 0, 0xFFFF9000, {0x10, 0x20, 0x30, 0x40}},
        Case{"sub_tcu_denso_sh7058_can", "SH7058", FlashOperation::kRead, 0, 0xFFFF3000, {0x50, 0x60, 0x70, 0x80}},
        Case{"sub_tcu_denso_sh7058_can",
             "SH7058",
             FlashOperation::kWrite,
             0x100000,
             0xFFFF3000,
             {0x50, 0x60, 0x70, 0x80}},
    };

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const Case& test : cases)
    {
        auto input = Request(test.protocol, test.operation);
        input.protocol.mcu = test.mcu;
        input.paths = *paths;
        if (test.image_size != 0)
        {
            input.image = bytes::Bytes(test.image_size, 0xa5);
        }
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << test.protocol;

        auto step = workflow->Next();
        if (const auto *failure = std::get_if<FlashFailureStep>(&step))
        {
            FAIL() << failure->error.detail.c_str();
        }
        ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
        ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.TargetId(), std::string_view(test.protocol));
        ASSERT_TRUE(plan.Operation() == test.operation);
        ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
        ASSERT_TRUE(plan.Kernel().has_value());
        ASSERT_EQ(plan.Kernel()->load_address, test.kernel_address);
        ASSERT_EQ(plan.Kernel()->bytes, test.kernel_bytes);
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
    constexpr std::array kCases{
        Case{"sub_tcu_denso_sh7055_can", "SH7055", FlashOperation::kWrite, 0x80000},
        Case{"sub_tcu_denso_sh7055_can", "SH7055", FlashOperation::kTestWrite, 0x80000},
        Case{"sub_tcu_denso_sh7058_can", "SH7058", FlashOperation::kTestWrite, 0x100000},
    };

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ExpectNoBackendIo(*fake);

    for (const Case& test : kCases)
    {
        auto input = Request(test.protocol, test.operation);
        input.protocol.mcu = test.mcu;
        input.paths = *paths;
        input.image = bytes::Bytes(test.image_size, 0xa5);
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << test.protocol;

        const auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kUnsupported);
    }
}

TEST(FlashWorkflowTest, tcuReadResolvesKernelBeforeBeginAndBindsDesktopCanTransport)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ExpectCanTransportSetup(*fake, false, 2017, 2025);

    auto input = Request("sub_tcu_denso_sh7055_can");
    input.protocol.mcu = "SH7055";
    input.paths = *paths;
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    ASSERT_TRUE(QFile::remove(directory.filePath("kernels/catalog_tcu_sh7055.bin")));

    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->Plan();
    ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
    ASSERT_TRUE(plan.Kernel().has_value());
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0x10, 0x20, 0x30, 0x40}));

    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(2);
    NullEventSink events;
    const auto result = attempt.attempt->Run(*attempt.clock, cancellation, events);
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error().kind, ErrorKind::kCancelled);
}

TEST(FlashWorkflowTest, tcuSuccessfulReadPropagatesBytesAndRomId)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto input = Request("sub_tcu_denso_sh7058_can");
    input.protocol.mcu = "SH7058";
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    workflow->Submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a, 0xa5}, .rom_id = "123456789A_"});
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kSucceeded);
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({0x5a, 0xa5}));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).rom_id, std::string("123456789A_"));
}

TEST(FlashWorkflowTest, mc68ResolvesKernelThroughCatalogBeforePromptAndAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Request("sub_ecu_denso_mc68hc16y5_02");
    input.protocol.mcu = "MC68HC16Y5";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        FAIL() << failure->error.detail.c_str();
    }
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);

    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_TRUE(plan.Kernel().has_value());
    ASSERT_EQ(plan.Kernel()->load_address, 0x20000U);
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0x11, 0x22, 0x33}));
}

TEST(FlashWorkflowTest, missingCatalogKernelFailsBeforePrompt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Request("sub_ecu_denso_mc68hc16y5_02");
    input.protocol.mcu = "MC68HC16Y5";
    const auto paths = CatalogPaths(directory, false);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
}

TEST(FlashWorkflowTest, sh7055IteratesConfirmationsAndPropagatesAttemptResult)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Request("sub_ecu_denso_sh7055_02");
    input.protocol.mcu = "SH7055";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kCycleIgnition);
    workflow->Submit(FlashPromptResponse::kAccept);

    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_TRUE(plan.Kernel().has_value());
    ASSERT_EQ(plan.Kernel()->load_address, 0xFFFF6004U);
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0xaa, 0xbb, 0xcc, 0xdd}));

    workflow->Submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a}, .rom_id = "123456789A_"});
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
    const auto& done = std::get<FlashCompletedStep>(step);
    ASSERT_EQ(done.outcome, FlashWorkflowOutcome::kSucceeded);
    ASSERT_EQ(done.accepted_read_bytes, bytes::Bytes({0x5a}));
    ASSERT_EQ(done.rom_id, std::string("123456789A_"));
}

TEST(FlashWorkflowTest, sh7055EcutekResolvesWithoutCarModelReference)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Request("sub_ecu_denso_sh7055_02_ecutek");
    input.protocol.mcu = "SH7055";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_EQ(plan.TargetId(), std::string_view("sub_ecu_denso_sh7055_02_ecutek"));
    ASSERT_TRUE(plan.Kernel().has_value());
    ASSERT_EQ(plan.Kernel()->load_address, 0xFFFF6004U);
}

TEST(FlashWorkflowTest, portableImageCopiesRomForEveryNonReadOperation)
{
    const bytes::Bytes rom{0x11, 0x22};
    ASSERT_TRUE(!PortableImageForOperation(FlashOperation::kRead, rom).has_value());
    ASSERT_EQ(PortableImageForOperation(FlashOperation::kWrite, rom), rom);
    ASSERT_EQ(PortableImageForOperation(FlashOperation::kTestWrite, rom), rom);
}

TEST(FlashWorkflowTest, mc68TestWriteWithPortableImageReachesAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Mc68Request(FlashOperation::kTestWrite);
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    const bytes::Bytes packed_image(0x28000, 0x5a);
    input.image = PortableImageForOperation(input.operation, packed_image);
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    ASSERT_EQ(std::get<FlashAttempt>(step).attempt->Plan().Image(), packed_image);
}

TEST(FlashWorkflowTest, mc68FullFileIsPlacedByItsCatalogMap)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Mc68Request(FlashOperation::kWrite);
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;

    bytes::Bytes full_file(0x30000, 0xee);
    std::fill_n(full_file.begin(), 0x20000, 0x11);
    std::fill(full_file.begin() + 0x28000, full_file.end(), 0x22);
    input.image = full_file;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        FAIL() << failure->error.detail.c_str();
    }
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const FlashPlan& plan = std::get<FlashAttempt>(step).attempt->Plan();
    // Handed over as the file it is, with the map for its size; not packed.
    ASSERT_EQ(plan.Image(), full_file);
    ASSERT_TRUE(plan.RomImage().has_value());
    EXPECT_EQ(plan.RomImage()->Map().FileSize(), memory::ByteCount{0x30000});
    EXPECT_EQ(RenderedFlash(plan, 0x00000, 0x20000), bytes::Bytes(0x20000, 0x11));
    EXPECT_EQ(RenderedFlash(plan, 0x28000, 0x8000), bytes::Bytes(0x8000, 0x22));
}

TEST(FlashWorkflowTest, mc68FullAndPackedFilesRenderTheSameFlash)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    bytes::Bytes packed_file(0x28000);
    for (std::size_t index = 0; index < packed_file.size(); ++index)
    {
        packed_file[index] = static_cast<bytes::Byte>((index / 0x4000) + 1);
    }
    // A 192 KiB file: the packed flash with 0x8000 bytes of 0xFF for the RAM
    // range at 0x20000, as a 192 KiB community file or a BDM read holds it.
    bytes::Bytes full_file = packed_file;
    full_file.insert(full_file.begin() + 0x20000, 0x8000, bytes::Byte{0xFF});

    std::vector<std::unique_ptr<FlashWorkflow>> workflows;
    std::vector<decltype(FlashAttempt::attempt)> attempts;
    for (bytes::Bytes file : {packed_file, full_file})
    {
        auto input = Mc68Request(FlashOperation::kTestWrite);
        input.paths = *paths;
        input.image = std::move(file);
        workflows.push_back(FlashWorkflowFactory::TryCreate(std::move(input)));
        ASSERT_TRUE(workflows.back() != nullptr);
        ASSERT_EQ(std::get<FlashPromptStep>(workflows.back()->Next()).kind, FlashPromptKind::kBegin);
        workflows.back()->Submit(FlashPromptResponse::kAccept);
        auto step = workflows.back()->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        attempts.push_back(std::move(std::get<FlashAttempt>(step).attempt));
    }
    const FlashPlan& packed_plan = attempts[0]->Plan();
    const FlashPlan& full_plan = attempts[1]->Plan();
    for (const auto& [start, size] : {std::pair{0x00000U, 0x20000U}, std::pair{0x28000U, 0x8000U}})
    {
        SCOPED_TRACE(start);
        EXPECT_EQ(RenderedFlash(packed_plan, start, size), RenderedFlash(full_plan, start, size));
        EXPECT_FALSE(RenderedFlash(packed_plan, start, size).empty());
    }
}

TEST(FlashWorkflowTest, mc68FileSizeWithoutAMapIsRefusedBeforeTheKernelIsRead)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Mc68Request(FlashOperation::kWrite);
    // No kernel files: reading the kernel first would report that instead.
    const auto paths = CatalogPaths(directory, /*include_kernel_files=*/false);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    input.image = bytes::Bytes(0x29000, 0x5a);
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    if (std::holds_alternative<FlashPromptStep>(step))
    {
        workflow->Submit(FlashPromptResponse::kAccept);
        step = workflow->Next();
    }
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    EXPECT_THAT(std::get<FlashFailureStep>(step).error.detail, ::testing::HasSubstr("ROM file size error"));
}

TEST(FlashWorkflowTest, sh7055TestWriteWithPortableImageReachesPromptsAndAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Request("sub_ecu_denso_sh7055_02", FlashOperation::kTestWrite);
    input.protocol.mcu = "SH7055";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    input.image = PortableImageForOperation(input.operation, bytes::Bytes(0x80000, 0xa5));
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kCycleIgnition);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
}

TEST(FlashWorkflowTest, mc68TpuReadResolvesCatalogAndReachesAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_tpu");
    input.protocol.mcu = "MC68HC16Y5_TPU";
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& kernel = std::get<FlashAttempt>(step).attempt->Plan().Kernel();
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
         FlashOperation::kRead,
         SubaruDensoSh705xKlineSeedKey::kStock,
         {0xaa, 0xbb, 0xcc, 0xdd}},
        {"sub_ecu_denso_sh7058_ecutek",
         "SH7058",
         FlashOperation::kRead,
         SubaruDensoSh705xKlineSeedKey::kEcuTek,
         {0x01, 0x02, 0x03, 0x04}},
        {"sub_ecu_denso_sh7058_cobb",
         "SH7058",
         FlashOperation::kTestWrite,
         SubaruDensoSh705xKlineSeedKey::kStock,
         {0x01, 0x02, 0x03, 0x04}},
    };
    for (const Case& c : cases)
    {
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const auto paths = CatalogPaths(directory);
        ASSERT_TRUE(paths.has_value());
        auto input = Request(c.protocol, c.operation);
        input.protocol.mcu = c.mcu;
        input.paths = *paths;
        if (c.operation != FlashOperation::kRead)
        {
            input.image = bytes::Bytes(std::size_t{1024} * 1024, 0xff);
        }
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << c.protocol;

        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << c.protocol;
        const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoSh705xKline);
        ASSERT_EQ(plan.TargetId(), std::string(c.protocol));
        ASSERT_TRUE(plan.Kernel().has_value());
        ASSERT_EQ(plan.Kernel()->bytes, c.kernel);
        ASSERT_EQ(std::get<SubaruDensoSh705xKlinePlan>(plan.FamilyPlan()).seed_key, c.seed_key);
    }
}

TEST(FlashWorkflowTest, densoSh705xKlineCobbReadFailsBeforeAttempt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto input = Request("sub_ecu_denso_sh7058_cobb");
    input.protocol.mcu = "SH7058";
    input.paths = *paths;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kUnsupported);
}

TEST(FlashWorkflowTest, densoSh705xKlineIgnoresPrefixLookalikes)
{
    for (const char *near_miss :
         {"sub_ecu_denso_sh7055_04_future", "sub_ecu_denso_sh7058_extra", "sub_ecu_denso_sh7058_ecutek_racerom"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(near_miss)) == nullptr) << near_miss;
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
        auto input = Request(variant.protocol);
        input.protocol.mcu = variant.mcu;
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << variant.protocol;
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << variant.protocol;
        const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruUnisiaJecsM32rKline);
        ASSERT_EQ(plan.TransferRegion(), (MemoryRegion{0x100000, variant.rom_size}));
    }
}

TEST(FlashWorkflowTest, unisiaJecsM32rLookalikesStayUnrouted)
{
    for (const char *protocol : {"sub_ecu_unisia_jecs_20x", "sub_ecu_unisia_jecs_7", "sub_ecu_unisia_jecs_20_bootmodex",
                                 "sub_ecu_unisia_jecs_40_bootmode"})
    {
        ASSERT_TRUE(FlashWorkflowFactory::TryCreate(Request(protocol)) == nullptr) << protocol;
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
        auto input = Request(variant.protocol);
        input.protocol.mcu = variant.mcu;
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << variant.protocol;
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << variant.protocol;
        const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruUnisiaJecsM32rKline);
        ASSERT_EQ(plan.TransferRegion(), (MemoryRegion{0x100000, variant.rom_size}));
        ASSERT_TRUE(plan.Confirmations().empty());
        workflow->Submit(
            FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1}, .rom_id = std::string("123456789A_")});
        const auto done = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
        ASSERT_TRUE(std::get<FlashCompletedStep>(done).rom_id == std::optional<std::string>("123456789A_"));
    }
}

TEST(FlashWorkflowTest, unisiaBootmodeWriteRunsKernelThenMod1ThenProgram)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = FlashWorkflowFactory::TryCreate(UnisiaBootmodeWrite(*paths));
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    if (const auto *failure = std::get_if<FlashFailureStep>(&step))
    {
        FAIL() << failure->error.detail.c_str();
    }
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kApplyBootModeVoltages);
    workflow->Submit(FlashPromptResponse::kAccept);

    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& kernel = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_EQ(kernel.Family(), FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel);
    bytes::Bytes padded{0x01, 0x02, 0x03, 0x04, 0x05};
    padded.resize(0x80, 0x00);
    ASSERT_EQ(kernel.Image(), std::optional<bytes::Bytes>(padded));
    workflow->Submit(FlashAttemptResult{.success = true});

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kRemoveMod1);
    workflow->Submit(FlashPromptResponse::kAccept);

    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& program = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_EQ(program.Family(), FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram);
    ASSERT_EQ(program.Image(), std::optional<bytes::Bytes>(bytes::Bytes(0x20000, 0xa5)));
    workflow->Submit(FlashAttemptResult{.success = true});

    const auto notice = std::get<FlashPromptStep>(workflow->Next());
    ASSERT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == BootmodeNotice("succeeded"));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kSucceeded);
}

TEST(FlashWorkflowTest, unisiaBootmodeKernelFailureSkipsMod1AndProgram)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = UnisiaBootmodeAtKernelAttempt(*paths);
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(
        FlashAttemptResult{.success = false, .error_kind = ErrorKind::kInvalidConfig, .error_detail = "baud"});
    const auto notice = std::get<FlashPromptStep>(workflow->Next());
    ASSERT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == BootmodeNotice("failed"));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto failure = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::kInvalidConfig);
}

TEST(FlashWorkflowTest, unisiaBootmodeKernelCancelledShowsNotice)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = UnisiaBootmodeAtKernelAttempt(*paths);
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::kCancelled});
    const auto notice = std::get<FlashPromptStep>(workflow->Next());
    ASSERT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == BootmodeNotice("cancelled"));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, unisiaBootmodeDeclinedMod1CancelsWithNotice)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = UnisiaBootmodeAtKernelAttempt(*paths);
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(FlashAttemptResult{.success = true});
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kRemoveMod1);
    workflow->Submit(FlashPromptResponse::kDecline);
    const auto notice = std::get<FlashPromptStep>(workflow->Next());
    ASSERT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == BootmodeNotice("cancelled"));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, unisiaBootmodeProgramFailureShowsNoticeThenFailure)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = UnisiaBootmodeAtKernelAttempt(*paths);
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(FlashAttemptResult{.success = true});
    workflow->Next(); // RemoveMod1
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    workflow->Submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::kBadResponse, .error_detail = "x"});
    const auto notice = std::get<FlashPromptStep>(workflow->Next());
    ASSERT_TRUE(notice.arguments == BootmodeNotice("failed"));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto failure = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::kBadResponse);
}

TEST(FlashWorkflowTest, unisiaBootmodeDeclinedVoltagesCancelsBeforeAnyAttempt)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = FlashWorkflowFactory::TryCreate(UnisiaBootmodeWrite(*paths));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kApplyBootModeVoltages);
    workflow->Submit(FlashPromptResponse::kDecline);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, unisiaBootmodeWrongImageSizeFailsBeforeAnyPrompt)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto input = UnisiaBootmodeWrite(*paths);
    input.image = bytes::Bytes(0x20001, 0xa5);
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
}

TEST(FlashWorkflowTest, unisiaBootmodeMissingKernelFailsBeforeAnyPrompt)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory, false);
    ASSERT_TRUE(paths.has_value());
    auto workflow = FlashWorkflowFactory::TryCreate(UnisiaBootmodeWrite(*paths));
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_TRUE(std::get<FlashFailureStep>(step).error.detail.find("catalog_uj20_bootmode.bin") != std::string::npos);
}

TEST(FlashWorkflowTest, unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    ASSERT_TRUE(WriteFile(directory.filePath("kernels/catalog_uj20_bootmode.bin"), QByteArray()));
    auto workflow = FlashWorkflowFactory::TryCreate(UnisiaBootmodeWrite(*paths));
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
}

TEST(FlashWorkflowTest, unisiaBootmodeTestWriteIsUnsupported)
{
    QTemporaryDir directory;
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto input = UnisiaBootmodeWrite(*paths);
    input.operation = FlashOperation::kTestWrite;
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kUnsupported);
}

TEST(FlashWorkflowTest, unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter)
{
    // request() carries a null serial: no adapter information means prompting.
    auto workflow = FlashWorkflowFactory::TryCreate(UnisiaM32rWrite());
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kApplyProgrammingVoltage);
    workflow->Submit(FlashPromptResponse::kAccept);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_EQ(plan.Confirmations().size(), std::size_t{1});
    ASSERT_EQ(plan.Confirmations()[0].id, ConfirmationSpec::Id::kApplyProgrammingVoltage);

    workflow->Submit(FlashAttemptResult{.success = true});
    const auto reminder = std::get<FlashPromptStep>(workflow->Next());
    ASSERT_EQ(reminder.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(reminder.arguments == (PromptArguments{{"outcome", "succeeded"}, {"external_vpp", "yes"}}));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kSucceeded);
}

TEST(FlashWorkflowTest, unisiaJecsM32rFailedWriteRemindsBeforeReportingTheFailure)
{
    auto workflow = UnisiaM32rWriteAtAttempt();
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::kBadResponse, .error_detail = "x"});
    const auto reminder = std::get<FlashPromptStep>(workflow->Next());
    ASSERT_EQ(reminder.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(reminder.arguments == (PromptArguments{{"outcome", "failed"}, {"external_vpp", "yes"}}));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto failure = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::kBadResponse);
}

TEST(FlashWorkflowTest, unisiaJecsM32rCancelledWriteReminds)
{
    auto workflow = UnisiaM32rWriteAtAttempt();
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::kCancelled});
    const auto reminder = std::get<FlashPromptStep>(workflow->Next());
    ASSERT_EQ(reminder.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(reminder.arguments == (PromptArguments{{"outcome", "cancelled"}, {"external_vpp", "yes"}}));
    workflow->Submit(FlashPromptResponse::kDecline); // OK-only notice; any answer continues
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, unisiaJecsM32rDeclinedVppPromptCancelsBeforeAttempt)
{
    auto workflow = FlashWorkflowFactory::TryCreate(UnisiaM32rWrite());
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kApplyProgrammingVoltage);
    workflow->Submit(FlashPromptResponse::kDecline);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, unisiaJecsM32rAdapterSuppliedVppSkipsBothPrompts)
{
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ASSERT_TRUE(serial->SetUseOpenport2Adapter(true));
    auto input = UnisiaM32rWrite();
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    ASSERT_TRUE(std::get<FlashAttempt>(step).attempt->Plan().Confirmations().empty());
    workflow->Submit(FlashAttemptResult{.success = true});
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kSucceeded);
}

// OpenPort 2.0 supplies VPP, so no remove-VPP sentence is due; legacy still
// warned on every failed write not to power off the ECU.
std::unique_ptr<FlashWorkflow> UnisiaM32rOpenPort2WriteAtAttempt(std::unique_ptr<SerialPortActions>& serial,
                                                                 FakeBackend **fake)
{
    serial = RecordingSerial(fake);
    if (serial == nullptr || !serial->SetUseOpenport2Adapter(true))
    {
        return nullptr;
    }
    auto input = UnisiaM32rWrite();
    input.serial = serial.get();
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    if (workflow == nullptr || std::get<FlashPromptStep>(workflow->Next()).kind != FlashPromptKind::kBegin)
    {
        return nullptr;
    }
    workflow->Submit(FlashPromptResponse::kAccept);
    if (!std::holds_alternative<FlashAttempt>(workflow->Next()))
    {
        return nullptr;
    }
    return workflow;
}

TEST(FlashWorkflowTest, unisiaJecsM32rAdapterSuppliedVppFailedWriteWarnsNotToPowerOff)
{
    FakeBackend *fake = nullptr;
    std::unique_ptr<SerialPortActions> serial;
    auto workflow = UnisiaM32rOpenPort2WriteAtAttempt(serial, &fake);
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::kTimeout, .error_detail = "x"});
    const auto notice_step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(notice_step));
    const auto& notice = std::get<FlashPromptStep>(notice_step);
    ASSERT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == (PromptArguments{{"outcome", "failed"}, {"external_vpp", "no"}}));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto failure = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::kTimeout);
}

TEST(FlashWorkflowTest, unisiaJecsM32rAdapterSuppliedVppCancelledWriteWarnsNotToPowerOff)
{
    FakeBackend *fake = nullptr;
    std::unique_ptr<SerialPortActions> serial;
    auto workflow = UnisiaM32rOpenPort2WriteAtAttempt(serial, &fake);
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::kCancelled});
    const auto notice_step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(notice_step));
    const auto& notice = std::get<FlashPromptStep>(notice_step);
    ASSERT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == (PromptArguments{{"outcome", "cancelled"}, {"external_vpp", "no"}}));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, unisiaJecsM32rReadPropagatesRomIdWithoutVppPrompts)
{
    auto input = Request("sub_ecu_unisia_jecs_30");
    input.protocol.mcu = "M32R_256KB";
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    workflow->Submit(
        FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1, 2}, .rom_id = std::string("123456789A_")});
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_TRUE(std::get<FlashCompletedStep>(done).rom_id == std::optional<std::string>("123456789A_"));
    ASSERT_TRUE(std::get<FlashCompletedStep>(done).accepted_read_bytes ==
                std::optional<bytes::Bytes>(bytes::Bytes{1, 2}));
}

TEST(FlashWorkflowTest, unisiaJecsM32rFailedReadReportsWithoutNotice)
{
    auto input = Request("sub_ecu_unisia_jecs_30");
    input.protocol.mcu = "M32R_256KB";
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    workflow->Submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::kTimeout, .error_detail = "x"});
    const auto failure = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::kTimeout);
}

TEST(FlashWorkflowTest, unisiaJecsM32rWriteOnReadOnlyVariantFailsBeforeAnyPrompt)
{
    auto input = Request("sub_ecu_unisia_jecs_40", FlashOperation::kWrite);
    input.protocol.mcu = "M32R_384KB";
    input.image = bytes::Bytes(0x60000, 0xff);
    auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kUnsupported);
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

KernelImage CatalogKernel(std::string_view protocol, std::uint32_t load_address, bytes::Bytes bytes)
{
    return KernelImage{
        .id = std::format("{}-kernel", protocol), .load_address = load_address, .bytes = std::move(bytes)};
}

std::vector<SingleAttemptCase> SingleAttemptCases()
{
    using enum FlashFamily;
    using enum TransportKind;
    return {
        {"sub_ecu_hitachi_m32r_can", "M32R_512KB_1block", kSubaruHitachiM32rCan, kCanIso15765, std::nullopt, {}},
        {"sub_tcu_cvt_hitachi_m32r_can", "M32R_512KB", kSubaruTcuCvtHitachiM32rCan, kCanIso15765, std::nullopt, {}},
        {"sub_tcu_cvt_mitsu_mh8111_can", "MH8111", kSubaruTcuCvtMitsuMh8111Can, kCanIso15765, std::nullopt, {}},
        {"sub_tcu_cvt_mitsu_mh8104_can", "MH8104", kSubaruTcuCvtMitsuMh8104Can, kCanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_1n83m_1_5m_can", "N83M_1_5MB", kSubaruDenso1n83m15mCan, kCanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_sh72531_can", "SH72531", kSubaruDensoSh72531Can, kCanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_sh72543_can_diesel", "SH72543d", kSubaruDensoSh72543CanDiesel, kCanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_1n83m_4m_can", "N83M_4MB", kSubaruDenso1n83m4mCan, kCanIso15765, std::nullopt, {}},
        {"sub_tcu_hitachi_m32r_can", "M32R_512KB", kSubaruTcuHitachiM32rCan, kCanIso15765, std::nullopt, {}},
        {"sub_ecu_hitachi_sh72543r_can", "SH72543R", kSubaruHitachiSh72543rCan, kCanIso15765, std::nullopt, {}},
        {"sub_ecu_mitsu_m32r_kline", "M32R_512KB_4blocks", kSubaruMitsuM32rKline, kKline, std::nullopt, {}},
        {"sub_ecu_hitachi_m32r_kline", "M32R_512KB_1block", kSubaruHitachiM32rKline, kKline, std::nullopt, {}},
        {"sub_tcu_hitachi_m32r_kline", "M32R_512KB", kSubaruTcuHitachiM32rKline, kKline, std::nullopt, {}},
        {"sub_ecu_unisia_jecs_m3779x", "M3779x", kSubaruUnisiaJecs, kKline, std::nullopt, {}},
        {"sub_ecu_hitachi_sh7058_can",
         "SH7058_1block",
         kSubaruHitachiSh7058,
         kKline,
         std::nullopt,
         {FlashPromptKind::kConfirmSh7058Read}},
        {"sub_ecu_denso_mc68hc16y5_02_bdm", "MC68HC16Y5", kSubaruDensoMc68hc16y502Bdm, kKline, std::nullopt, {}},
        {"mitsu_ecu_m32r_can", "M32R_384KB_1block", kMitsuColtM32rCan, kCanIso15765, std::nullopt, {}},
        {"sub_ecu_denso_sh7055_densocan",
         "SH7055",
         kSubaruDensoSh705xDensoCan,
         kCanRawIso15765,
         CatalogKernel("sub_ecu_denso_sh7055_densocan", 0xFFFF6004, {0xaa, 0xbb, 0xcc, 0xdd}),
         {FlashPromptKind::kCycleIgnition}},
        {"sub_tcu_denso_sh7055_can",
         "SH7055",
         kSubaruTcuDensoSh705xCan,
         kCanIso15765,
         CatalogKernel("sub_tcu_denso_sh7055_can", 0xFFFF9000, {0x10, 0x20, 0x30, 0x40}),
         {}},
        {"sub_ecu_denso_sh7058_can",
         "SH7058",
         kSubaruDensoSh7058Can,
         kCanIso15765,
         CatalogKernel("sub_ecu_denso_sh7058_can", 0xFFFF3000, {0x90, 0xa0, 0xb0, 0xc0}),
         {}},
        {"sub_ecu_denso_sh7058_can_diesel",
         "SH7058d",
         kSubaruDensoSh7058CanDiesel,
         kCanIso15765,
         CatalogKernel("sub_ecu_denso_sh7058_can_diesel", 0xFFFF4000, {0xd0, 0xe0, 0xf0, 0x01}),
         {}},
        {"sub_ecu_denso_sh7055_04",
         "SH7055",
         kSubaruDensoSh705xKline,
         kKline,
         CatalogKernel("sub_ecu_denso_sh7055_04", 0xFFFF6004, {0xaa, 0xbb, 0xcc, 0xdd}),
         {}},
        {"sub_ecu_denso_sh7055_02",
         "SH7055",
         kSubaruDensoSh705502,
         kKline,
         CatalogKernel("sub_ecu_denso_sh7055_02", 0xFFFF6004, {0xaa, 0xbb, 0xcc, 0xdd}),
         {FlashPromptKind::kCycleIgnition}},
        {"sub_ecu_denso_mc68hc16y5_02",
         "MC68HC16Y5",
         kSubaruDensoMc68hc16y502,
         kKline,
         CatalogKernel("sub_ecu_denso_mc68hc16y5_02", 0x20000, {0x11, 0x22, 0x33}),
         {}},
    };
}

FlashWorkflowRequest SingleAttemptRead(const SingleAttemptCase& test, const config::ConfigPaths& paths)
{
    auto input = Request(test.protocol);
    input.protocol.mcu = test.mcu;
    input.paths = paths;
    return input;
}

std::vector<FlashPromptKind> PromptKinds(const std::vector<FlashPromptStep>& prompts)
{
    std::vector<FlashPromptKind> kinds;
    std::ranges::transform(prompts, std::back_inserter(kinds), &FlashPromptStep::kind);
    return kinds;
}

std::vector<FlashPromptKind> WithBegin(const std::vector<FlashPromptKind>& confirmations)
{
    std::vector<FlashPromptKind> kinds{FlashPromptKind::kBegin};
    kinds.insert(kinds.end(), confirmations.begin(), confirmations.end());
    return kinds;
}

// Accepts every prompt, recording it, and returns the first step that is not
// a prompt. The bound keeps a workflow that never stops prompting from
// hanging the suite.
FlashWorkflowStep AcceptEveryPrompt(FlashWorkflow& workflow, std::vector<FlashPromptStep>& prompts)
{
    for (int bound = 0; bound < 8; ++bound)
    {
        FlashWorkflowStep step = workflow.Next();
        const auto *prompt = std::get_if<FlashPromptStep>(&step);
        if (prompt == nullptr)
        {
            return step;
        }
        prompts.push_back(*prompt);
        workflow.Submit(FlashPromptResponse::kAccept);
    }
    return FlashFailureStep{Error{ErrorKind::kInternal, "workflow kept prompting"}};
}

std::string FailureDetail(const FlashWorkflowStep& step)
{
    const auto *failure = std::get_if<FlashFailureStep>(&step);
    return failure == nullptr ? std::string() : failure->error.detail;
}

TEST(FlashWorkflowTest, singleAttemptFamiliesPromptInOrderAndBindTheirExecutorOnce)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);
        auto workflow = FlashWorkflowFactory::TryCreate(SingleAttemptRead(test, *paths));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = AcceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
        EXPECT_THAT(PromptKinds(prompts), ::testing::ElementsAreArray(WithBegin(test.confirmations)));
        EXPECT_THAT(prompts, ::testing::Each(::testing::Field(&FlashPromptStep::arguments, ::testing::IsEmpty())));

        auto& attempt = std::get<FlashAttempt>(step);
        ASSERT_TRUE(attempt.attempt != nullptr);
        ASSERT_TRUE(attempt.clock != nullptr);
        const FlashPlan& plan = attempt.attempt->Plan();
        EXPECT_EQ(plan.Family(), test.family);
        EXPECT_EQ(plan.Transport(), test.transport);
        EXPECT_EQ(plan.TargetId(), test.protocol);
        EXPECT_EQ(plan.McuName(), test.mcu);
        EXPECT_EQ(plan.Operation(), FlashOperation::kRead);
        EXPECT_EQ(plan.Kernel().has_value(), test.kernel.has_value());

        // transport_setup() rejects a plan from another family, so reaching
        // the pre-configure cancellation proves the bound executor owns this
        // plan. Nothing is configured or opened on the way.
        FakeCancellationToken cancelled(true);
        NullEventSink events;
        EXPECT_THAT(attempt.attempt->Run(*attempt.clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::HasSubstr("before configure")));

        // Exactly one attempt: neither the pending nor the finished workflow
        // hands out a second one.
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->Next()));
        workflow->Submit(FlashAttemptResult{.success = true});
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->Next()));
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    }
}

TEST(FlashWorkflowTest, singleAttemptFamiliesCancelWhenAnyPromptIsDeclined)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        const std::vector<FlashPromptKind> sequence = WithBegin(test.confirmations);
        for (std::size_t declined = 0; declined < sequence.size(); ++declined)
        {
            SCOPED_TRACE(std::format("{} declining prompt {}", test.protocol, declined));
            auto workflow = FlashWorkflowFactory::TryCreate(SingleAttemptRead(test, *paths));
            ASSERT_TRUE(workflow != nullptr);
            for (std::size_t accepted = 0; accepted < declined; ++accepted)
            {
                ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, sequence[accepted]);
                workflow->Submit(FlashPromptResponse::kAccept);
            }
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, sequence[declined]);
            workflow->Submit(FlashPromptResponse::kDecline);

            for (int repeat = 0; repeat < 2; ++repeat)
            {
                const auto step = workflow->Next();
                ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
                const auto& done = std::get<FlashCompletedStep>(step);
                EXPECT_EQ(done.outcome, FlashWorkflowOutcome::kCancelled);
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
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        for (const auto response : {FlashPromptResponse::kSave, FlashPromptResponse::kDiscard})
        {
            SCOPED_TRACE(std::format("{} response {}", test.protocol, static_cast<int>(response)));
            auto workflow = FlashWorkflowFactory::TryCreate(SingleAttemptRead(test, *paths));
            ASSERT_TRUE(workflow != nullptr);
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
            workflow->Submit(response);
            const auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kCancelled);
        }
    }
}

std::unique_ptr<FlashWorkflow> SingleAttemptAtAttempt(const SingleAttemptCase& test, const config::ConfigPaths& paths)
{
    auto workflow = FlashWorkflowFactory::TryCreate(SingleAttemptRead(test, paths));
    std::vector<FlashPromptStep> prompts;
    if (workflow == nullptr || !std::holds_alternative<FlashAttempt>(AcceptEveryPrompt(*workflow, prompts)))
    {
        return nullptr;
    }
    return workflow;
}

TEST(FlashWorkflowTest, singleAttemptFamiliesReportEveryAttemptOutcome)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);

        auto with_identity = SingleAttemptAtAttempt(test, *paths);
        ASSERT_TRUE(with_identity != nullptr);
        with_identity->Submit(
            FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a, 0xa5}, .rom_id = "CAL_123456789A_"});
        auto step = with_identity->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kSucceeded);
        EXPECT_EQ(std::get<FlashCompletedStep>(step).accepted_read_bytes, bytes::Bytes({0x5a, 0xa5}));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).rom_id, std::string("CAL_123456789A_"));

        auto without_identity = SingleAttemptAtAttempt(test, *paths);
        ASSERT_TRUE(without_identity != nullptr);
        without_identity->Submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x01}});
        step = without_identity->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kSucceeded);
        EXPECT_EQ(std::get<FlashCompletedStep>(step).accepted_read_bytes, bytes::Bytes({0x01}));
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).rom_id.has_value());

        auto failed = SingleAttemptAtAttempt(test, *paths);
        ASSERT_TRUE(failed != nullptr);
        failed->Submit(FlashAttemptResult{.success = false,
                                          .error_kind = ErrorKind::kTimeout,
                                          .error_detail = "no reply to 0x34",
                                          .read_bytes = bytes::Bytes{0x02},
                                          .rom_id = "IGNORED"});
        step = failed->Next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::kTimeout, "no reply to 0x34"}));

        auto cancelled = SingleAttemptAtAttempt(test, *paths);
        ASSERT_TRUE(cancelled != nullptr);
        cancelled->Submit(FlashAttemptResult{.success = false,
                                             .error_kind = ErrorKind::kCancelled,
                                             .error_detail = "cancelled: dialog closed",
                                             .read_bytes = bytes::Bytes{0x03}});
        step = cancelled->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kCancelled);
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).accepted_read_bytes.has_value());
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).rom_id.has_value());
    }
}

TEST(FlashWorkflowTest, singleAttemptFamiliesRejectInvalidConfigurationBeforeAnyPromptOrIo)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ExpectNoBackendIo(*fake);

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);
        auto input = SingleAttemptRead(test, *paths);
        input.protocol.mcu = "NOT_A_KNOWN_MCU";
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
        }
    }
}

TEST(FlashWorkflowTest, kernelBackedFamiliesFailBeforeAnyPromptOrIoWithoutTheirKernel)
{
    QTemporaryDir without_kernels;
    ASSERT_TRUE(without_kernels.isValid());
    const auto catalog_only = CatalogPaths(without_kernels, false);
    ASSERT_TRUE(catalog_only.has_value());
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ExpectNoBackendIo(*fake);

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        if (!test.kernel.has_value())
        {
            continue;
        }
        for (const bool has_catalog : {true, false})
        {
            SCOPED_TRACE(std::format("{} with{} catalog", test.protocol, has_catalog ? "" : "out"));
            auto input = SingleAttemptRead(test, has_catalog ? *catalog_only : config::ConfigPaths{});
            input.serial = serial.get();
            auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
            ASSERT_TRUE(workflow != nullptr);
            for (int repeat = 0; repeat < 2; ++repeat)
            {
                const auto step = workflow->Next();
                ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
                EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
            }
        }
    }
}

TEST(FlashWorkflowTest, kernelBackedFamiliesKeepTheirKernelSnapshotAfterFilesAreRemoved)
{
    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        if (!test.kernel.has_value())
        {
            continue;
        }
        SCOPED_TRACE(test.protocol);
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const auto paths = CatalogPaths(directory);
        ASSERT_TRUE(paths.has_value());
        auto workflow = FlashWorkflowFactory::TryCreate(SingleAttemptRead(test, *paths));
        ASSERT_TRUE(workflow != nullptr);

        // The kernel is loaded before Begin; nothing on disk is read afterward.
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        ASSERT_TRUE(QDir(directory.filePath("kernels")).removeRecursively());
        workflow->Submit(FlashPromptResponse::kAccept);

        std::vector<FlashPromptStep> prompts;
        auto step = AcceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
        EXPECT_THAT(PromptKinds(prompts), ::testing::ElementsAreArray(test.confirmations));
        const auto& kernel = std::get<FlashAttempt>(step).attempt->Plan().Kernel();
        ASSERT_TRUE(kernel.has_value());
        EXPECT_EQ(kernel->id, test.kernel->id);
        EXPECT_EQ(kernel->load_address, test.kernel->load_address);
        EXPECT_EQ(kernel->bytes, test.kernel->bytes);
    }
}

// Hitachi SH7058 Write runs over CAN with Begin as its only prompt; its Read
// side (K-Line) is in singleAttemptCases().
FlashWorkflowRequest Sh7058Write()
{
    auto input = Request("sub_ecu_hitachi_sh7058_can", FlashOperation::kWrite);
    input.protocol.mcu = "SH7058_1block";
    input.image = bytes::Bytes(0x100000, 0x5a);
    return input;
}

TEST(FlashWorkflowTest, sh7058WriteBindsTheCanExecutorAfterBeginAlone)
{
    auto workflow = FlashWorkflowFactory::TryCreate(Sh7058Write());
    ASSERT_TRUE(workflow != nullptr);

    std::vector<FlashPromptStep> prompts;
    auto step = AcceptEveryPrompt(*workflow, prompts);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
    EXPECT_THAT(PromptKinds(prompts), ::testing::ElementsAre(FlashPromptKind::kBegin));

    auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->Plan();
    EXPECT_EQ(plan.Family(), FlashFamily::kSubaruHitachiSh7058);
    EXPECT_EQ(plan.Transport(), TransportKind::kCanIso15765);
    EXPECT_EQ(plan.Operation(), FlashOperation::kWrite);
    EXPECT_EQ(plan.Image(), std::optional<bytes::Bytes>(bytes::Bytes(0x100000, 0x5a)));
    EXPECT_TRUE(plan.Confirmations().empty());

    // The K-Line executor's transport_setup() rejects a Write plan as
    // Unsupported, so reaching the pre-configure cancellation proves the CAN
    // executor is bound. Nothing is configured or opened on the way.
    FakeCancellationToken cancelled(true);
    NullEventSink events;
    EXPECT_THAT(attempt.attempt->Run(*attempt.clock, cancelled, events),
                fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::HasSubstr("before configure")));

    workflow->Submit(
        FlashAttemptResult{.success = false, .error_kind = ErrorKind::kTimeout, .error_detail = "no reply to 0x34"});
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::kTimeout, "no reply to 0x34"}));
}

TEST(FlashWorkflowTest, sh7058WriteCancelsWhenBeginIsDeclined)
{
    auto workflow = FlashWorkflowFactory::TryCreate(Sh7058Write());
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kDecline);
    for (int repeat = 0; repeat < 2; ++repeat)
    {
        const auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kCancelled);
    }
}

TEST(FlashWorkflowTest, sh7058WriteRejectsInvalidInputBeforeAnyPromptOrIo)
{
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ExpectNoBackendIo(*fake);

    struct Case
    {
        const char *name;
        FlashOperation operation;
        std::size_t image_size;
        ErrorKind expected;
    };
    for (const Case& test : std::to_array<Case>({
             {"test write", FlashOperation::kTestWrite, 0x100000, ErrorKind::kUnsupported},
             {"short image", FlashOperation::kWrite, 0xFFFFF, ErrorKind::kInvalidConfig},
         }))
    {
        SCOPED_TRACE(test.name);
        auto input = Sh7058Write();
        input.operation = test.operation;
        input.image = bytes::Bytes(test.image_size, 0x5a);
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, test.expected);
        }
    }
}

// Denso MC68HC16Y5 BDM Write uploads the catalog kernel to RAM and starts it.
// The desktop hands every Write the operator's ROM; BDM must drop it.
FlashWorkflowRequest BdmWrite(const config::ConfigPaths& paths)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::kWrite);
    input.protocol.mcu = "MC68HC16Y5";
    input.paths = paths;
    input.image = bytes::Bytes(0x30000, 0x5a);
    return input;
}

// catalog_mc68.bin (11 22 33) zero-padded to the 0x20-byte upload chunk.
bytes::Bytes BdmCatalogKernelImage()
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
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const bool succeeded : {true, false})
    {
        SCOPED_TRACE(succeeded ? "succeeded" : "failed");
        auto workflow = FlashWorkflowFactory::TryCreate(BdmWrite(*paths));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = AcceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
        EXPECT_THAT(PromptKinds(prompts),
                    ::testing::ElementsAre(FlashPromptKind::kBegin, FlashPromptKind::kConfirmBdmKernelBootstrap));
        EXPECT_THAT(prompts, ::testing::Each(::testing::Field(&FlashPromptStep::arguments, ::testing::IsEmpty())));

        auto& attempt = std::get<FlashAttempt>(step);
        const FlashPlan& plan = attempt.attempt->Plan();
        EXPECT_EQ(plan.Family(), FlashFamily::kSubaruDensoMc68hc16y502Bdm);
        EXPECT_EQ(plan.Transport(), TransportKind::kKline);
        EXPECT_EQ(plan.Operation(), FlashOperation::kWrite);
        EXPECT_EQ(plan.Image(), std::optional<bytes::Bytes>(BdmCatalogKernelImage()));
        EXPECT_EQ(plan.TransferRegion(), (MemoryRegion{0x20000, 0x20}));
        EXPECT_FALSE(plan.Kernel().has_value());

        // transport_setup() validates the plan, so reaching the pre-configure
        // cancellation proves the BDM executor owns it.
        FakeCancellationToken cancelled(true);
        NullEventSink events;
        EXPECT_THAT(attempt.attempt->Run(*attempt.clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::HasSubstr("before configure")));
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->Next()));

        if (succeeded)
        {
            workflow->Submit(FlashAttemptResult{.success = true});
            step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kSucceeded);
            EXPECT_FALSE(std::get<FlashCompletedStep>(step).accepted_read_bytes.has_value());
        }
        else
        {
            workflow->Submit(FlashAttemptResult{
                .success = false, .error_kind = ErrorKind::kDisconnected, .error_detail = "adapter removed"});
            step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::kDisconnected, "adapter removed"}));
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteCancelsWhenEitherPromptIsDeclined)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    const std::vector<FlashPromptKind> sequence{FlashPromptKind::kBegin, FlashPromptKind::kConfirmBdmKernelBootstrap};

    for (std::size_t declined = 0; declined < sequence.size(); ++declined)
    {
        SCOPED_TRACE(std::format("declining prompt {}", declined));
        auto workflow = FlashWorkflowFactory::TryCreate(BdmWrite(*paths));
        ASSERT_TRUE(workflow != nullptr);
        for (std::size_t accepted = 0; accepted < declined; ++accepted)
        {
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, sequence[accepted]);
            workflow->Submit(FlashPromptResponse::kAccept);
        }
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, sequence[declined]);
        workflow->Submit(FlashPromptResponse::kDecline);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kCancelled);
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteWithoutItsKernelFailsBeforeAnyPromptOrIo)
{
    QTemporaryDir without_kernels;
    ASSERT_TRUE(without_kernels.isValid());
    const auto catalog_only = CatalogPaths(without_kernels, false);
    ASSERT_TRUE(catalog_only.has_value());
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ExpectNoBackendIo(*fake);

    for (const bool has_catalog : {true, false})
    {
        SCOPED_TRACE(has_catalog ? "with catalog" : "without catalog");
        auto input = BdmWrite(has_catalog ? *catalog_only : config::ConfigPaths{});
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            const auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteKeepsItsKernelSnapshotAfterFilesAreRemoved)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paths = CatalogPaths(directory);
    ASSERT_TRUE(paths.has_value());
    auto workflow = FlashWorkflowFactory::TryCreate(BdmWrite(*paths));
    ASSERT_TRUE(workflow != nullptr);

    // The kernel is loaded before Begin; nothing on disk is read afterward.
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    ASSERT_TRUE(QDir(directory.filePath("kernels")).removeRecursively());
    workflow->Submit(FlashPromptResponse::kAccept);

    std::vector<FlashPromptStep> prompts;
    auto step = AcceptEveryPrompt(*workflow, prompts);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
    EXPECT_THAT(PromptKinds(prompts), ::testing::ElementsAre(FlashPromptKind::kConfirmBdmKernelBootstrap));
    EXPECT_EQ(std::get<FlashAttempt>(step).attempt->Plan().Image(),
              std::optional<bytes::Bytes>(BdmCatalogKernelImage()));
}

struct ColtWriteCase
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t capacity;
    std::vector<FlashPromptStep> prompts;
};

std::vector<ColtWriteCase> ColtWriteCases()
{
    return {
        {"mitsu_ecu_m32r_can",
         "M32R_384KB_1block",
         0x60000,
         {{FlashPromptKind::kBegin, {}},
          {FlashPromptKind::kColtEraseTrigger,
           {{"capacity_kib", "384"}, {"writable_start_hex", "0x8000"}, {"rom_end_hex", "0x60000"}}}}},
        {"mitsu_ecu_m32r_can_vendor_ext_512kb",
         "M32R_512KB_1block",
         0x80000,
         {{FlashPromptKind::kBegin, {}},
          {FlashPromptKind::kColtEraseTrigger,
           {{"capacity_kib", "512"}, {"writable_start_hex", "0x8000"}, {"rom_end_hex", "0x80000"}}},
          {FlashPromptKind::kColtTopRegionBootstrap,
           {{"top_region_start_hex", "0x60000"}, {"rom_end_hex", "0x80000"}}}}},
    };
}

FlashWorkflowRequest ColtWrite(const ColtWriteCase& test)
{
    auto input = Request(test.protocol, FlashOperation::kWrite);
    input.protocol.mcu = test.mcu;
    input.image = bytes::Bytes(test.capacity, 0x5a);
    return input;
}

MATCHER_P(IsPrompt, expected, "")
{
    return arg.kind == expected.kind && arg.arguments == expected.arguments;
}

TEST(FlashWorkflowTest, coltWritePromptsCarryTheirCapacityArgumentsInOrder)
{
    for (const ColtWriteCase& test : ColtWriteCases())
    {
        SCOPED_TRACE(test.protocol);
        auto workflow = FlashWorkflowFactory::TryCreate(ColtWrite(test));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = AcceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
        ASSERT_EQ(prompts.size(), test.prompts.size());
        for (std::size_t index = 0; index < prompts.size(); ++index)
        {
            EXPECT_THAT(prompts[index], IsPrompt(test.prompts[index])) << "prompt " << index;
        }

        auto& attempt = std::get<FlashAttempt>(step);
        const FlashPlan& plan = attempt.attempt->Plan();
        EXPECT_EQ(plan.Family(), FlashFamily::kMitsuColtM32rCan);
        EXPECT_EQ(plan.Transport(), TransportKind::kCanIso15765);
        EXPECT_EQ(plan.Operation(), FlashOperation::kWrite);
        EXPECT_EQ(plan.TransferRegion(), (MemoryRegion{0x8000, test.capacity - 0x8000}));
        EXPECT_EQ(plan.Image(), std::optional<bytes::Bytes>(bytes::Bytes(test.capacity, 0x5a)));
        EXPECT_EQ(plan.Confirmations().size(), test.prompts.size() - 1);

        FakeCancellationToken cancelled(true);
        NullEventSink events;
        EXPECT_THAT(attempt.attempt->Run(*attempt.clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::HasSubstr("before configure")));

        workflow->Submit(FlashAttemptResult{.success = true});
        step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kSucceeded);
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).accepted_read_bytes.has_value());
        EXPECT_FALSE(std::get<FlashCompletedStep>(step).rom_id.has_value());
    }
}

TEST(FlashWorkflowTest, coltWriteCancelsWhenAnyPromptIsDeclined)
{
    for (const ColtWriteCase& test : ColtWriteCases())
    {
        for (std::size_t declined = 0; declined < test.prompts.size(); ++declined)
        {
            SCOPED_TRACE(std::format("{} declining prompt {}", test.protocol, declined));
            auto workflow = FlashWorkflowFactory::TryCreate(ColtWrite(test));
            ASSERT_TRUE(workflow != nullptr);
            for (std::size_t accepted = 0; accepted < declined; ++accepted)
            {
                ASSERT_THAT(std::get<FlashPromptStep>(workflow->Next()), IsPrompt(test.prompts[accepted]));
                workflow->Submit(FlashPromptResponse::kAccept);
            }
            ASSERT_THAT(std::get<FlashPromptStep>(workflow->Next()), IsPrompt(test.prompts[declined]));
            workflow->Submit(FlashPromptResponse::kDecline);
            for (int repeat = 0; repeat < 2; ++repeat)
            {
                const auto step = workflow->Next();
                ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
                EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kCancelled);
            }
        }
    }
}

TEST(FlashWorkflowTest, coltWriteWithWrongImageSizeFailsBeforeAnyPromptOrIo)
{
    FakeBackend *fake = nullptr;
    auto serial = RecordingSerial(&fake);
    ASSERT_TRUE(serial != nullptr);
    ExpectNoBackendIo(*fake);

    for (const ColtWriteCase& test : ColtWriteCases())
    {
        SCOPED_TRACE(test.protocol);
        auto input = ColtWrite(test);
        input.image = bytes::Bytes(test.capacity + 1, 0x5a);
        input.serial = serial.get();
        auto workflow = FlashWorkflowFactory::TryCreate(std::move(input));
        ASSERT_TRUE(workflow != nullptr);
        const auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
    }
}

} // namespace
} // namespace fastecu::flash

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
