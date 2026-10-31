#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/workflow/flash_workflow.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <gmock/gmock-matchers.h>

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
#include "src/backend/config/config_paths.h"
#include "src/backend/flash/testing/scripted_flash_transport_source.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"
#include "src/backend/flash/flash_transport_source.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/null_event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"

using ::testing::_;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Optional;

namespace fastecu::flash
{
namespace
{

// The kernel each protocol declared in the synthetic catalog these tests
// used to write as protocols.cfg; PopulateCatalogKernels() supplies the files.
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
    return {
        .operation = operation, .protocol = spec, .image = std::nullopt, .paths = {}, .display_filename = "test.bin"};
}

config::ConfigPaths CatalogPaths()
{
    return {.kernel_files_directory = "kernels/"};
}

void PopulateCatalogKernels(InMemoryFileRepository& files)
{
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_mc68.bin")] = {0x11, 0x22, 0x33};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_tpu.bin")] = {0x44, 0x55, 0x66};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_sh7055.bin")] = {0xaa, 0xbb, 0xcc, 0xdd};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_densocan.bin")] = {0xaa, 0xbb, 0xcc, 0xdd};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_tcu_sh7055.bin")] = {0x10, 0x20, 0x30, 0x40};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_tcu_sh7058.bin")] = {0x50, 0x60, 0x70, 0x80};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_petrol_sh7058.bin")] = {0x90, 0xa0, 0xb0, 0xc0};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_diesel_sh7058.bin")] = {0xd0, 0xe0, 0xf0, 0x01};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_diesel_sh7059.bin")] = {0xd0, 0xe0, 0xf0, 0x02};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_kline_sh7055.bin")] = {0xaa, 0xbb, 0xcc, 0xdd};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_kline_sh7058.bin")] = {0x01, 0x02, 0x03, 0x04};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_uj20_bootmode.bin")] = {0x01, 0x02, 0x03, 0x04, 0x05};
    files.files[config::KernelFileHandle(CatalogPaths(), "catalog_uj30_bootmode.bin")] = {0x06, 0x07};
}

struct WorkflowTestPorts
{
    explicit WorkflowTestPorts(bool include_kernel_files = true, bool supplies_programming_voltage = false)
    {
        auto repository = std::make_unique<InMemoryFileRepository>();
        files = repository.get();
        if (include_kernel_files)
        {
            PopulateCatalogKernels(*files);
        }
        auto source = std::make_unique<ScriptedFlashTransportSource>(supplies_programming_voltage);
        transports = source.get();
        ports = {.files = std::move(repository), .transports = std::move(source)};
    }

    FlashWorkflowPorts ports;
    InMemoryFileRepository *files;
    ScriptedFlashTransportSource *transports;
};

// Consumes the bundle once. Observers remain valid while the workflow owns its ports.
std::unique_ptr<FlashWorkflow> CreateWorkflow(FlashWorkflowRequest request, WorkflowTestPorts& test_ports)
{
    return FlashWorkflowFactory::TryCreate(std::move(request), std::move(test_ports.ports));
}

std::unique_ptr<FlashWorkflow> CreateWorkflow(FlashWorkflowRequest request)
{
    WorkflowTestPorts test_ports;
    return CreateWorkflow(std::move(request), test_ports);
}

// Preloads stay observable whether requested or still queued in the source.
struct PreflightTransportObservers
{
    explicit PreflightTransportObservers(WorkflowTestPorts& test_ports)
    {
        auto kline_transport = std::make_unique<ScriptedKlineFlashTransport>();
        kline = kline_transport.get();
        test_ports.transports->QueueKline(std::move(kline_transport));
        auto can_transport = std::make_unique<ScriptedCanFlashTransport>();
        can = can_transport.get();
        test_ports.transports->QueueCan(std::move(can_transport));
        auto mixed_transport = std::make_unique<ScriptedMixedCanFlashTransport>();
        mixed = mixed_transport.get();
        test_ports.transports->QueueMixedCan(std::move(mixed_transport));
    }

    void ExpectNoIo() const
    {
        EXPECT_THAT(kline->last_config, Eq(std::nullopt));
        EXPECT_THAT(can->last_config, Eq(std::nullopt));
        EXPECT_THAT(mixed->last_config, Eq(std::nullopt));
        EXPECT_THAT(kline->lifecycle_calls, ::testing::SizeIs(0));
        EXPECT_THAT(can->lifecycle_calls, ::testing::SizeIs(0));
        EXPECT_EQ(mixed->ConfigureCallCount(), 0);
        EXPECT_EQ(mixed->OpenCallCount(), 0);
        EXPECT_EQ(mixed->reset_connection_call_count, 0);
        EXPECT_TRUE(kline->ScriptConsumed());
        EXPECT_TRUE(can->ScriptConsumed());
        EXPECT_TRUE(mixed->ScriptConsumed());
    }

    ScriptedKlineFlashTransport *kline;
    ScriptedCanFlashTransport *can;
    ScriptedMixedCanFlashTransport *mixed;
};

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
    auto workflow = CreateWorkflow(UnisiaM32rWrite());
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
    auto workflow = CreateWorkflow(UnisiaBootmodeWrite(paths));
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

// The built-in catalog's protocols that flag at least one flash operation.
std::vector<const config::ProtocolSpec *> CatalogFlashProtocols()
{
    std::vector<const config::ProtocolSpec *> specs;
    for (const config::ProtocolSpec& spec : config::BuiltinCatalog().Protocols())
    {
        if (spec.read || spec.test_write || spec.write)
        {
            specs.push_back(&spec);
        }
    }
    return specs;
}

// Accepts every prompt and stops at the first attempt, failure or completion.
FlashWorkflowStep DriveToAttemptOrFailure(FlashWorkflow& workflow)
{
    constexpr int kMaxSteps = 16;
    FlashWorkflowStep step = workflow.Next();
    for (int taken = 1; taken < kMaxSteps && std::holds_alternative<FlashPromptStep>(step); ++taken)
    {
        workflow.Submit(FlashPromptResponse::kAccept);
        step = workflow.Next();
    }
    return step;
}

TEST(FlashRouteTest, everyCatalogFlashProtocolHasExactlyOneRoute)
{
    const std::vector<std::string_view> routed = FlashWorkflowFactory::RoutedProtocolIds();
    for (const config::ProtocolSpec *spec : CatalogFlashProtocols())
    {
        SCOPED_TRACE(spec->name);
        EXPECT_EQ(std::ranges::count(routed, spec->name), 1);
    }
}

TEST(FlashRouteTest, everyRoutedIdIsACatalogFlashProtocol)
{
    for (const std::string_view id : FlashWorkflowFactory::RoutedProtocolIds())
    {
        SCOPED_TRACE(id);
        const config::ProtocolSpec *spec = config::BuiltinCatalog().FindProtocol(id);
        ASSERT_NE(spec, nullptr);
        EXPECT_TRUE(spec->read || spec->test_write || spec->write);
    }
}

TEST(FlashRouteTest, everyFlashProtocolRoutesToAWorkflowThatAcceptsIt)
{
    for (const config::ProtocolSpec *spec : CatalogFlashProtocols())
    {
        SCOPED_TRACE(spec->name);
        FlashOperation operation = FlashOperation::kRead;
        if (!spec->read)
        {
            operation = spec->test_write ? FlashOperation::kTestWrite : FlashOperation::kWrite;
        }
        FlashWorkflowRequest request{.operation = operation,
                                     .protocol = *spec,
                                     .image = std::nullopt,
                                     .paths = CatalogPaths(),
                                     .display_filename = "test.bin"};
        if (operation != FlashOperation::kRead)
        {
            request.image = bytes::Bytes(1, 0xff);
        }
        WorkflowTestPorts test_ports(false);
        if (!spec->kernel.empty())
        {
            test_ports.files->files[config::KernelFileHandle(CatalogPaths(), spec->kernel)] = {0x01, 0x02, 0x03, 0x04};
        }
        auto workflow = CreateWorkflow(std::move(request), test_ports);
        ASSERT_NE(workflow, nullptr);
        const FlashWorkflowStep step = DriveToAttemptOrFailure(*workflow);
        const auto *failure = std::get_if<FlashFailureStep>(&step);
        if (operation == FlashOperation::kRead)
        {
            EXPECT_TRUE(std::holds_alternative<FlashAttempt>(step))
                << (failure != nullptr ? failure->error.detail : std::string("drive stopped before an attempt"));
        }
        else if (failure != nullptr)
        {
            // Not every catalog protocol flags Read (e.g. sub_ecu_denso_sh7055_04_cobb is test-write only).
            EXPECT_NE(failure->error.kind, ErrorKind::kUnsupported) << failure->error.detail;
        }
    }
}

TEST(FlashRouteTest, lookalikesOutsideTheCatalogHaveNoRoute)
{
    for (const char *name :
         {"mitsu_ecu_m32r_can_typo", "sub_ecu_denso_mc68hc16y5_02_bdm_x", "sub_ecu_unisia_jecs_20_bootmode_x",
          "sub_ecu_denso_sh7055_04_x", "sub_ecu_eeprom_denso_sh7058_can_x", "mitsu_ecu_m32r_kline_mut_dma",
          "sub_ecu_denso_sh7055_04_future", "sub_ecu_denso_sh7058_extra", "sub_ecu_denso_sh7058_ecutek_racerom"})
    {
        SCOPED_TRACE(name);
        EXPECT_EQ(CreateWorkflow(Request(name)), nullptr);
    }
}

TEST(FlashWorkflowTest, preflightPrecedesPromptsAndDeclineCancels)
{
    auto invalid = Request("mitsu_ecu_m32r_can", FlashOperation::kTestWrite);
    auto workflow = CreateWorkflow(std::move(invalid));
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(workflow->Next()));

    workflow = CreateWorkflow(Request("mitsu_ecu_m32r_can"));
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kDecline);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, successfulReadBytesAreAcceptedAutomatically)
{
    auto workflow = CreateWorkflow(Request("mitsu_ecu_m32r_can"));
    std::ignore = workflow->Next();
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    workflow->Submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1, 2, 3}});
    auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).accepted_read_bytes, bytes::Bytes({1, 2, 3}));
}

TEST(FlashWorkflowTest, unisiaJecsReadPlansCarryTheirProtocolAndMcu)
{
    static constexpr auto kPairs = std::to_array<std::pair<const char *, const char *>>({
        {"sub_ecu_unisia_jecs_m3779x", "M3779x"},
        {"sub_ecu_unisia_jecs_m3775x", "M3775x"},
    });

    for (const auto& [protocol, mcu] : kPairs)
    {
        SCOPED_TRACE(::testing::PrintToString(protocol));
        auto input = Request(protocol);
        input.protocol.mcu = mcu;
        auto workflow = CreateWorkflow(std::move(input));
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
}

TEST(FlashWorkflowTest, unisiaJecsCrossPairsFailBeforeAttempt)
{
    static constexpr auto kCrossPairs = std::to_array<std::pair<const char *, const char *>>({
        {"sub_ecu_unisia_jecs_m3779x", "M3775x"},
        {"sub_ecu_unisia_jecs_m3775x", "M3779x"},
    });

    for (const auto& [protocol, mcu] : kCrossPairs)
    {
        SCOPED_TRACE(::testing::PrintToString(protocol));
        auto input = Request(protocol);
        input.protocol.mcu = mcu;
        auto workflow = CreateWorkflow(std::move(input));
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
    auto workflow = CreateWorkflow(std::move(input));
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
        SCOPED_TRACE(::testing::PrintToString(protocol));
        auto input = Request(protocol);
        input.protocol.mcu = "M32R_512KB_1block";
        auto workflow = CreateWorkflow(std::move(input));
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
    auto workflow = CreateWorkflow(std::move(input));
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
    auto write_workflow = CreateWorkflow(std::move(write_request));
    ASSERT_TRUE(write_workflow != nullptr);
    const auto write_step = write_workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(write_step));
    ASSERT_EQ(std::get<FlashFailureStep>(write_step).error.kind, ErrorKind::kUnsupported);
}

TEST(FlashWorkflowTest, routesTcuHitachiM32rCanReadAndWriteRejectsTestWrite)
{
    // The default request() helper's MCU ("M32R_384KB_1block") is not this
    // family's kMcu ("M32R_512KB"); BuildSubaruTcuHitachiM32rCanPlan's
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
    auto read_workflow = CreateWorkflow(std::move(read_input));
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
    auto test_write_workflow = CreateWorkflow(std::move(test_write_input));
    ASSERT_TRUE(test_write_workflow != nullptr);
    const auto test_write_step = test_write_workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(test_write_step));
    ASSERT_EQ(std::get<FlashFailureStep>(test_write_step).error.kind, ErrorKind::kUnsupported);

    // Write, unlike the K-Line sibling, is supported by this family and
    // routes all the way to an attempt bound to the CAN executor/transport.
    auto write_input = Request(kProtocol, FlashOperation::kWrite);
    write_input.protocol.mcu = kMcu;
    write_input.image = bytes::Bytes(0x80000, 0xa5);
    auto write_workflow = CreateWorkflow(std::move(write_input));
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
        SCOPED_TRACE(::testing::PrintToString(protocol));
        for (auto operation : {FlashOperation::kRead, FlashOperation::kWrite})
        {
            SCOPED_TRACE(::testing::PrintToString(operation));
            auto input = Request(protocol, operation);
            input.protocol.mcu = "SH72543R";
            if (operation == FlashOperation::kWrite)
            {
                input.image = bytes::Bytes(0x200000, 0xa5);
            }
            auto workflow = CreateWorkflow(std::move(input));
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
                ASSERT_THAT(done.accepted_read_bytes, Eq(std::nullopt));
                ASSERT_THAT(done.rom_id, Eq(std::nullopt));
            }
        }
    }
    ASSERT_TRUE(!CreateWorkflow(Request("sub_ecu_hitachi_sh72543r_can_recovery_typo")));
    ASSERT_TRUE(!CreateWorkflow(Request("sub_ecu_hitachi_sh72543r_can_typo")));
}
TEST(FlashWorkflowTest, routesSh7058ReadAndWriteWithPreTransportPrompts)
{
    ASSERT_TRUE(!CreateWorkflow(Request("sub_ecu_hitachi_sh7058_can_extra")));
    for (const auto operation : {FlashOperation::kRead, FlashOperation::kWrite})
    {
        SCOPED_TRACE(::testing::PrintToString(operation));
        auto input = Request("sub_ecu_hitachi_sh7058_can", operation);
        input.protocol.mcu = "SH7058_1block";
        if (operation == FlashOperation::kWrite)
        {
            input.image = bytes::Bytes(0x100000, 0x5a);
        }
        WorkflowTestPorts test_ports;
        auto workflow = CreateWorkflow(std::move(input), test_ports);
        EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::SizeIs(0));
        ASSERT_TRUE(workflow);
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::SizeIs(0));
        if (operation == FlashOperation::kRead)
        {
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kConfirmSh7058Read);
            EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::SizeIs(0));
            workflow->Submit(FlashPromptResponse::kAccept);
        }
        auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
        EXPECT_THAT(test_ports.transports->RequestedKinds(),
                    ::testing::ElementsAre(operation == FlashOperation::kRead ? TransportKind::kKline
                                                                              : TransportKind::kCanIso15765));
        const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruHitachiSh7058);
        ASSERT_EQ(plan.Transport(),
                  operation == FlashOperation::kRead ? TransportKind::kKline : TransportKind::kCanIso15765);
    }
    auto input = Request("sub_ecu_hitachi_sh7058_can");
    input.protocol.mcu = "SH7058_1block";
    WorkflowTestPorts test_ports;
    auto workflow = CreateWorkflow(std::move(input), test_ports);
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::SizeIs(0));
    workflow->Submit(FlashPromptResponse::kAccept);
    workflow->Submit(FlashPromptResponse::kDecline);
    ASSERT_EQ(std::get<FlashCompletedStep>(workflow->Next()).outcome, FlashWorkflowOutcome::kCancelled);
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::SizeIs(0));
}
TEST(FlashWorkflowTest, sh72543rRejectsPreflightAndDeclinedBegin)
{
    for (const char *protocol : {"sub_ecu_hitachi_sh72543r_can", "sub_ecu_hitachi_sh72543r_can_recovery"})
    {
        SCOPED_TRACE(::testing::PrintToString(protocol));
        for (int fault = 0; fault < 3; ++fault)
        {
            SCOPED_TRACE(::testing::PrintToString(fault));
            auto input = Request(protocol, fault == 0 ? FlashOperation::kTestWrite : FlashOperation::kWrite);
            input.protocol.mcu = fault == 1 ? "SH72543d" : "SH72543R";
            input.image = bytes::Bytes(fault == 2 ? 16 : 0x200000);
            auto workflow = CreateWorkflow(std::move(input));
            ASSERT_TRUE(workflow);
            auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind,
                      fault == 0 ? ErrorKind::kUnsupported : ErrorKind::kInvalidConfig);
        }
        auto input = Request(protocol);
        input.protocol.mcu = "SH72543R";
        auto workflow = CreateWorkflow(std::move(input));
        ASSERT_TRUE(workflow);
        ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(workflow->Next()));
        workflow->Submit(FlashPromptResponse::kDecline);
        auto done = std::get<FlashCompletedStep>(workflow->Next());
        ASSERT_EQ(done.outcome, FlashWorkflowOutcome::kCancelled);
        ASSERT_THAT(done.accepted_read_bytes, Eq(std::nullopt));
    }
}
TEST(FlashWorkflowTest, sh72543rPropagatesFailureAndAbsentIdentity)
{
    for (int outcome = 0; outcome < 3; ++outcome)
    {
        SCOPED_TRACE(::testing::PrintToString(outcome));
        auto input = Request("sub_ecu_hitachi_sh72543r_can");
        input.protocol.mcu = "SH72543R";
        auto workflow = CreateWorkflow(std::move(input));
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
            ASSERT_THAT(done.rom_id, Eq(std::nullopt));
            ASSERT_EQ(done.outcome, outcome == 0 ? FlashWorkflowOutcome::kSucceeded : FlashWorkflowOutcome::kCancelled);
        }
    }
}

TEST(FlashWorkflowTest, coltWriteUsesColtSpecificSafetyPrompts)
{
    auto write = Request("mitsu_ecu_m32r_can", FlashOperation::kWrite);
    write.image = bytes::Bytes(0x60000);
    auto workflow = CreateWorkflow(std::move(write));

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kColtEraseTrigger);
}

TEST(FlashWorkflowTest, mc68BdmReadRoutesThroughBeginToAttempt)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm");
    input.protocol.mcu = "MC68HC16Y5";
    auto workflow = CreateWorkflow(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoMc68hc16y502Bdm);
    ASSERT_EQ(plan.Transport(), TransportKind::kKline);
    ASSERT_EQ(plan.TransferRegion(), (MemoryRegion{0, 0x30000}));
    ASSERT_THAT(plan.Image(), Eq(std::nullopt));
}

TEST(FlashWorkflowTest, mc68BdmWriteBootstrapsTheCatalogKernelNotTheRom)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::kWrite);
    input.protocol.mcu = "MC68HC16Y5";
    const auto paths = CatalogPaths();

    input.paths = paths;
    input.image = bytes::Bytes(0x30000, 0x5a);
    auto workflow = CreateWorkflow(std::move(input));
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
    ASSERT_THAT(plan.Kernel(), Eq(std::nullopt));
}

TEST(FlashWorkflowTest, mc68BdmDeclinedBootstrapConfirmationCancels)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_bdm", FlashOperation::kWrite);
    input.protocol.mcu = "MC68HC16Y5";
    const auto paths = CatalogPaths();

    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input));
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
    auto workflow = CreateWorkflow(std::move(input));
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
    auto workflow = CreateWorkflow(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kUnsupported);
}

TEST(FlashWorkflowTest, densoCanResolvesKernelPromptsAndPropagatesAttemptResult)
{
    auto input = Request("sub_ecu_denso_sh7055_densocan");
    input.protocol.mcu = "SH7055";
    const auto paths = CatalogPaths();

    input.paths = paths;
    WorkflowTestPorts test_ports;
    auto workflow = CreateWorkflow(std::move(input), test_ports);
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    test_ports.files->files.erase(config::KernelFileHandle(paths, "catalog_densocan.bin"));
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kCycleIgnition);
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
    ASSERT_EQ(plan.Transport(), TransportKind::kCanRawIso15765);
    ASSERT_THAT(plan.Kernel(), Optional(_));
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
    const auto paths = CatalogPaths();

    auto input = Request("sub_ecu_denso_sh7055_densocan");
    input.protocol.mcu = "SH7055";
    input.paths = paths;
    input.protocol.kernel_load_address.reset();

    auto workflow = CreateWorkflow(std::move(input));
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
    auto workflow = CreateWorkflow(std::move(missing_kernel));
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(workflow->Next()));

    for (const bool decline_begin : {true, false})
    {
        SCOPED_TRACE(::testing::PrintToString(decline_begin));
        auto input = Request("sub_ecu_denso_sh7055_densocan");
        input.protocol.mcu = "SH7055";
        const auto paths = CatalogPaths();

        input.paths = paths;
        workflow = CreateWorkflow(std::move(input));
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

    const auto paths = CatalogPaths();

    for (const Case& test : cases)
    {
        SCOPED_TRACE(::testing::PrintToString(test));
        auto input = Request(test.protocol, test.operation);
        input.protocol.mcu = "SH7058";
        input.paths = paths;
        if (test.operation != FlashOperation::kRead)
        {
            input.image = bytes::Bytes(0x00100000, bytes::Byte{0xA5});
        }
        auto workflow = CreateWorkflow(std::move(input));
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
        ASSERT_THAT(plan.Confirmations(), IsEmpty());
        ASSERT_THAT(plan.Kernel(), Optional(_));
        ASSERT_EQ(plan.Kernel()->load_address, 0xFFFF3000U);
        ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0x90, 0xA0, 0xB0, 0xC0}));
        const auto *family_plan = plan.FamilyPlan().As<SubaruDensoSh7058CanPlan>();
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
    auto input = Request("sub_ecu_denso_sh7058_can");
    input.protocol.mcu = "SH7058";
    const auto paths = CatalogPaths();

    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input));
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

TEST(FlashWorkflowTest, petrolReadResolvesKernelBeforeBeginAndBindsCanTransport)
{
    const auto paths = CatalogPaths();

    WorkflowTestPorts test_ports;
    auto can = std::make_unique<ScriptedCanFlashTransport>();
    auto *can_observer = can.get();
    test_ports.transports->QueueCan(std::move(can));

    auto input = Request("sub_ecu_denso_sh7058_can");
    input.protocol.mcu = "SH7058";
    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input), test_ports);
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    // Resolution happened before Begin; removing the kernel now must not
    // affect the already-bound attempt.
    test_ports.files->files.erase(config::KernelFileHandle(paths, "catalog_petrol_sh7058.bin"));

    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->Plan();
    ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoSh7058Can);
    ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
    ASSERT_EQ(plan.TargetId(), std::string_view("sub_ecu_denso_sh7058_can"));
    ASSERT_THAT(plan.Kernel(), Optional(_));
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0x90, 0xA0, 0xB0, 0xC0}));

    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(5);
    NullEventSink events;
    FakeClock clock;
    const auto result = attempt.attempt->Run(clock, cancellation, events);
    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(TransportKind::kCanIso15765));
    ASSERT_THAT(can_observer->last_config, Optional(_));
    EXPECT_EQ(can_observer->last_config->bitrate, 500000);
    EXPECT_EQ(can_observer->last_config->request_id, 0x7E0U);
    EXPECT_EQ(can_observer->last_config->response_id, 0x7E8U);
    EXPECT_FALSE(can_observer->last_config->extended_id);
    EXPECT_THAT(can_observer->lifecycle_calls,
                ::testing::ElementsAre("reset_connection", "configure", "open", "close"));
    EXPECT_THAT(can_observer->ReadTimeouts(), ::testing::SizeIs(0));
    EXPECT_TRUE(can_observer->ScriptConsumed());
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
        bytes::Bytes kernel_bytes;
    };
    const std::array cases{
        Case{"sub_ecu_denso_sh7058_can_diesel", "SH7058d", FlashOperation::kRead, 0x00100000, 0xFFFF4000,
             bytes::Bytes{0xd0, 0xe0, 0xf0, 0x01}},
        Case{"sub_ecu_denso_sh7058_can_diesel", "SH7058d", FlashOperation::kTestWrite, 0x00100000, 0xFFFF4000,
             bytes::Bytes{0xd0, 0xe0, 0xf0, 0x01}},
        Case{"sub_ecu_denso_sh7058_can_diesel", "SH7058d", FlashOperation::kWrite, 0x00100000, 0xFFFF4000,
             bytes::Bytes{0xd0, 0xe0, 0xf0, 0x01}},
        Case{"sub_ecu_denso_sh7059_can_diesel", "SH7059d", FlashOperation::kRead, 0x00180000, 0xFFFEE000,
             bytes::Bytes{0xd0, 0xe0, 0xf0, 0x02}},
        Case{"sub_ecu_denso_sh7059_can_diesel", "SH7059d", FlashOperation::kTestWrite, 0x00180000, 0xFFFEE000,
             bytes::Bytes{0xd0, 0xe0, 0xf0, 0x02}},
        Case{"sub_ecu_denso_sh7059_can_diesel", "SH7059d", FlashOperation::kWrite, 0x00180000, 0xFFFEE000,
             bytes::Bytes{0xd0, 0xe0, 0xf0, 0x02}},
    };

    const auto paths = CatalogPaths();

    for (const Case& test : cases)
    {
        SCOPED_TRACE(::testing::PrintToString(test));
        auto input = Request(test.protocol, test.operation);
        input.protocol.mcu = test.mcu;
        input.paths = paths;
        if (test.operation != FlashOperation::kRead)
        {
            input.image = bytes::Bytes(test.rom_size, bytes::Byte{0xA5});
        }
        auto workflow = CreateWorkflow(std::move(input));
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
        ASSERT_THAT(plan.Confirmations(), IsEmpty());
        ASSERT_THAT(plan.Kernel(), Optional(_));
        ASSERT_EQ(plan.Kernel()->load_address, test.kernel_address);
        ASSERT_EQ(plan.Kernel()->bytes, test.kernel_bytes);
        const auto *family_plan = plan.FamilyPlan().As<SubaruDensoSh7058CanDieselPlan>();
        ASSERT_TRUE(family_plan != nullptr);
        ASSERT_EQ(family_plan->request_id, 0x7E0U);
        ASSERT_EQ(family_plan->response_id, 0x7E8U);
        ASSERT_EQ(family_plan->bitrate, 500000);
        ASSERT_TRUE(!family_plan->extended_id);
    }
}

TEST(FlashWorkflowTest, dieselSuccessfulReadPropagatesKernelSnapshotBytesAndRomId)
{
    auto input = Request("sub_ecu_denso_sh7059_can_diesel");
    input.protocol.mcu = "SH7059d";
    const auto paths = CatalogPaths();

    input.paths = paths;
    WorkflowTestPorts test_ports;
    auto workflow = CreateWorkflow(std::move(input), test_ports);
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    test_ports.files->files.erase(config::KernelFileHandle(paths, "catalog_diesel_sh7059.bin"));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto attempt_step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(attempt_step));
    const FlashPlan& snapshot = std::get<FlashAttempt>(attempt_step).attempt->Plan();
    ASSERT_THAT(snapshot.Kernel(), Optional(_));
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

TEST(FlashWorkflowTest, dieselReadResolvesKernelBeforeBeginAndBindsCanTransport)
{
    const auto paths = CatalogPaths();

    WorkflowTestPorts test_ports;
    auto can = std::make_unique<ScriptedCanFlashTransport>();
    auto *can_observer = can.get();
    test_ports.transports->QueueCan(std::move(can));

    auto input = Request("sub_ecu_denso_sh7059_can_diesel");
    input.protocol.mcu = "SH7059d";
    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input), test_ports);
    ASSERT_TRUE(workflow != nullptr);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);

    // The workflow owns the resolved kernel before Begin; this also proves
    // the Diesel route binds its CAN executor.
    test_ports.files->files.erase(config::KernelFileHandle(paths, "catalog_diesel_sh7059.bin"));
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->Plan();
    ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoSh7058CanDiesel);
    ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
    ASSERT_EQ(plan.TargetId(), std::string_view("sub_ecu_denso_sh7059_can_diesel"));
    ASSERT_EQ(plan.McuName(), std::string_view("SH7059d"));
    ASSERT_THAT(plan.Kernel(), Optional(_));
    ASSERT_EQ(plan.Kernel()->load_address, 0xFFFEE000U);
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0xD0, 0xE0, 0xF0, 0x02}));

    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(5);
    NullEventSink events;
    FakeClock clock;
    const auto result = attempt.attempt->Run(clock, cancellation, events);
    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(TransportKind::kCanIso15765));
    ASSERT_THAT(can_observer->last_config, Optional(_));
    EXPECT_EQ(can_observer->last_config->bitrate, 500000);
    EXPECT_EQ(can_observer->last_config->request_id, 0x7E0U);
    EXPECT_EQ(can_observer->last_config->response_id, 0x7E8U);
    EXPECT_FALSE(can_observer->last_config->extended_id);
    EXPECT_THAT(can_observer->lifecycle_calls,
                ::testing::ElementsAre("reset_connection", "configure", "open", "close"));
    EXPECT_THAT(can_observer->ReadTimeouts(), ::testing::SizeIs(0));
    EXPECT_TRUE(can_observer->ScriptConsumed());
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

    const auto paths = CatalogPaths();

    for (const Case& test : cases)
    {
        SCOPED_TRACE(::testing::PrintToString(test));
        auto input = Request(test.protocol, test.operation);
        input.protocol.mcu = test.mcu;
        input.paths = paths;
        if (test.image_size != 0)
        {
            input.image = bytes::Bytes(test.image_size, 0xa5);
        }
        auto workflow = CreateWorkflow(std::move(input));
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
        ASSERT_THAT(plan.Kernel(), Optional(_));
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

    const auto paths = CatalogPaths();

    for (const Case& test : kCases)
    {
        SCOPED_TRACE(::testing::PrintToString(test));
        auto input = Request(test.protocol, test.operation);
        input.protocol.mcu = test.mcu;
        input.paths = paths;
        input.image = bytes::Bytes(test.image_size, 0xa5);
        WorkflowTestPorts test_ports;
        PreflightTransportObservers observers(test_ports);
        auto workflow = CreateWorkflow(std::move(input), test_ports);
        ASSERT_TRUE(workflow != nullptr) << test.protocol;

        const auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kUnsupported);
        observers.ExpectNoIo();
    }
}

TEST(FlashWorkflowTest, tcuReadResolvesKernelBeforeBeginAndBindsCanTransport)
{
    const auto paths = CatalogPaths();

    WorkflowTestPorts test_ports;
    auto can = std::make_unique<ScriptedCanFlashTransport>();
    auto *can_observer = can.get();
    test_ports.transports->QueueCan(std::move(can));

    auto input = Request("sub_tcu_denso_sh7055_can");
    input.protocol.mcu = "SH7055";
    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input), test_ports);
    ASSERT_TRUE(workflow != nullptr);

    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(step));
    ASSERT_EQ(std::get<FlashPromptStep>(step).kind, FlashPromptKind::kBegin);
    test_ports.files->files.erase(config::KernelFileHandle(paths, "catalog_tcu_sh7055.bin"));

    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    auto& attempt = std::get<FlashAttempt>(step);
    const FlashPlan& plan = attempt.attempt->Plan();
    ASSERT_EQ(plan.Transport(), TransportKind::kCanIso15765);
    ASSERT_THAT(plan.Kernel(), Optional(_));
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0x10, 0x20, 0x30, 0x40}));

    FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(2);
    NullEventSink events;
    FakeClock clock;
    const auto result = attempt.attempt->Run(clock, cancellation, events);
    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(TransportKind::kCanIso15765));
    ASSERT_THAT(can_observer->last_config, Optional(_));
    EXPECT_EQ(can_observer->last_config->bitrate, 500000);
    EXPECT_EQ(can_observer->last_config->request_id, 0x7E1U);
    EXPECT_EQ(can_observer->last_config->response_id, 0x7E9U);
    EXPECT_FALSE(can_observer->last_config->extended_id);
    EXPECT_THAT(can_observer->lifecycle_calls, ::testing::ElementsAre("configure", "open", "close"));
    EXPECT_THAT(can_observer->ReadTimeouts(), ::testing::SizeIs(0));
    EXPECT_TRUE(can_observer->ScriptConsumed());
}

TEST(FlashWorkflowTest, tcuSuccessfulReadPropagatesBytesAndRomId)
{
    const auto paths = CatalogPaths();

    auto input = Request("sub_tcu_denso_sh7058_can");
    input.protocol.mcu = "SH7058";
    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input));
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
    auto input = Request("sub_ecu_denso_mc68hc16y5_02");
    input.protocol.mcu = "MC68HC16Y5";
    const auto paths = CatalogPaths();

    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input));
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
    ASSERT_THAT(plan.Kernel(), Optional(_));
    ASSERT_EQ(plan.Kernel()->load_address, 0x20000U);
    ASSERT_EQ(plan.Kernel()->bytes, bytes::Bytes({0x11, 0x22, 0x33}));
}

TEST(FlashWorkflowTest, missingCatalogKernelFailsBeforePrompt)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02");
    input.protocol.mcu = "MC68HC16Y5";
    const auto paths = CatalogPaths();

    input.paths = paths;
    WorkflowTestPorts test_ports(false);
    auto workflow = CreateWorkflow(std::move(input), test_ports);
    ASSERT_TRUE(workflow != nullptr);

    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
}

TEST(FlashWorkflowTest, sh7055IteratesConfirmationsAndPropagatesAttemptResult)
{
    auto input = Request("sub_ecu_denso_sh7055_02");
    input.protocol.mcu = "SH7055";
    const auto paths = CatalogPaths();

    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input));
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
    ASSERT_THAT(plan.Kernel(), Optional(_));
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
    auto input = Request("sub_ecu_denso_sh7055_02_ecutek");
    input.protocol.mcu = "SH7055";
    const auto paths = CatalogPaths();

    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input));
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
    ASSERT_THAT(plan.Kernel(), Optional(_));
    ASSERT_EQ(plan.Kernel()->load_address, 0xFFFF6004U);
}

TEST(FlashWorkflowTest, portableImageCopiesRomForEveryNonReadOperation)
{
    const bytes::Bytes rom{0x11, 0x22};
    ASSERT_THAT(PortableImageForOperation(FlashOperation::kRead, rom), Eq(std::nullopt));
    ASSERT_EQ(PortableImageForOperation(FlashOperation::kWrite, rom), rom);
    ASSERT_EQ(PortableImageForOperation(FlashOperation::kTestWrite, rom), rom);
}

TEST(FlashWorkflowTest, mc68TestWriteWithPortableImageReachesAttempt)
{
    auto input = Mc68Request(FlashOperation::kTestWrite);
    const auto paths = CatalogPaths();

    input.paths = paths;
    const bytes::Bytes packed_image(0x28000, 0x5a);
    input.image = PortableImageForOperation(input.operation, packed_image);
    auto workflow = CreateWorkflow(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    ASSERT_EQ(std::get<FlashAttempt>(step).attempt->Plan().Image(), packed_image);
}

TEST(FlashWorkflowTest, mc68FullFileIsPlacedByItsCatalogMap)
{
    auto input = Mc68Request(FlashOperation::kWrite);
    const auto paths = CatalogPaths();

    input.paths = paths;

    bytes::Bytes full_file(0x30000, 0xee);
    std::fill_n(full_file.begin(), 0x20000, 0x11);
    std::fill(full_file.begin() + 0x28000, full_file.end(), 0x22);
    input.image = full_file;
    auto workflow = CreateWorkflow(std::move(input));
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
    ASSERT_THAT(plan.RomImage(), Optional(_));
    EXPECT_EQ(plan.RomImage()->Map().FileSize(), memory::ByteCount{0x30000});
    EXPECT_THAT(RenderedFlash(plan, 0x00000, 0x20000),
                ::testing::AllOf(::testing::SizeIs(0x20000), test_bytes::BytesFilledWith(0x11)));
    EXPECT_THAT(RenderedFlash(plan, 0x28000, 0x8000),
                ::testing::AllOf(::testing::SizeIs(0x8000), test_bytes::BytesFilledWith(0x22)));
}

TEST(FlashWorkflowTest, mc68FullAndPackedFilesRenderTheSameFlash)
{
    const auto paths = CatalogPaths();

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
        SCOPED_TRACE(::testing::PrintToString(file));
        auto input = Mc68Request(FlashOperation::kTestWrite);
        input.paths = paths;
        input.image = std::move(file);
        workflows.push_back(CreateWorkflow(std::move(input)));
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
        EXPECT_THAT(RenderedFlash(packed_plan, start, size), Not(IsEmpty()));
    }
}

TEST(FlashWorkflowTest, mc68FileSizeWithoutAMapIsRefusedBeforeTheKernelIsRead)
{
    auto input = Mc68Request(FlashOperation::kWrite);
    // No kernel files: reading the kernel first would report that instead.
    const auto paths = CatalogPaths();

    input.paths = paths;
    input.image = bytes::Bytes(0x29000, 0x5a);
    WorkflowTestPorts test_ports(false);
    auto workflow = CreateWorkflow(std::move(input), test_ports);
    ASSERT_TRUE(workflow != nullptr);

    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    EXPECT_THAT(std::get<FlashFailureStep>(step).error.detail, ::testing::HasSubstr("ROM file size error"));
}

TEST(FlashWorkflowTest, sh7055TestWriteWithPortableImageReachesPromptsAndAttempt)
{
    auto input = Request("sub_ecu_denso_sh7055_02", FlashOperation::kTestWrite);
    input.protocol.mcu = "SH7055";
    const auto paths = CatalogPaths();

    input.paths = paths;
    input.image = PortableImageForOperation(input.operation, bytes::Bytes(0x80000, 0xa5));
    auto workflow = CreateWorkflow(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kCycleIgnition);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
}

TEST(FlashWorkflowTest, mc68TpuReadResolvesCatalogAndReachesAttempt)
{
    auto input = Request("sub_ecu_denso_mc68hc16y5_02_tpu");
    input.protocol.mcu = "MC68HC16Y5_TPU";
    const auto paths = CatalogPaths();

    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input));
    ASSERT_TRUE(workflow != nullptr);

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    const auto& kernel = std::get<FlashAttempt>(step).attempt->Plan().Kernel();
    ASSERT_THAT(kernel, Optional(_));
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
        SCOPED_TRACE(::testing::PrintToString(c));
        const auto paths = CatalogPaths();

        auto input = Request(c.protocol, c.operation);
        input.protocol.mcu = c.mcu;
        input.paths = paths;
        if (c.operation != FlashOperation::kRead)
        {
            input.image = bytes::Bytes(std::size_t{1024} * 1024, 0xff);
        }
        auto workflow = CreateWorkflow(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << c.protocol;

        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << c.protocol;
        const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruDensoSh705xKline);
        ASSERT_EQ(plan.TargetId(), std::string(c.protocol));
        ASSERT_THAT(plan.Kernel(), Optional(_));
        ASSERT_EQ(plan.Kernel()->bytes, c.kernel);
        ASSERT_EQ(plan.FamilyPlan().Get<SubaruDensoSh705xKlinePlan>().seed_key, c.seed_key);
    }
}

TEST(FlashWorkflowTest, densoSh705xKlineCobbReadFailsBeforeAttempt)
{
    const auto paths = CatalogPaths();

    auto input = Request("sub_ecu_denso_sh7058_cobb");
    input.protocol.mcu = "SH7058";
    input.paths = paths;
    auto workflow = CreateWorkflow(std::move(input));
    ASSERT_TRUE(workflow != nullptr);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kUnsupported);
}

TEST(FlashWorkflowTest, unisiaJecsM32rReadPlansTransferTheirRomSize)
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
        SCOPED_TRACE(::testing::PrintToString(variant));
        auto input = Request(variant.protocol);
        input.protocol.mcu = variant.mcu;
        auto workflow = CreateWorkflow(std::move(input));
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
        SCOPED_TRACE(::testing::PrintToString(variant));
        auto input = Request(variant.protocol);
        input.protocol.mcu = variant.mcu;
        auto workflow = CreateWorkflow(std::move(input));
        ASSERT_TRUE(workflow != nullptr) << variant.protocol;
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        workflow->Submit(FlashPromptResponse::kAccept);
        auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << variant.protocol;
        const auto& plan = std::get<FlashAttempt>(step).attempt->Plan();
        ASSERT_EQ(plan.Family(), FlashFamily::kSubaruUnisiaJecsM32rKline);
        ASSERT_EQ(plan.TransferRegion(), (MemoryRegion{0x100000, variant.rom_size}));
        ASSERT_THAT(plan.Confirmations(), IsEmpty());
        workflow->Submit(
            FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{1}, .rom_id = std::string("123456789A_")});
        const auto done = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
        ASSERT_TRUE(std::get<FlashCompletedStep>(done).rom_id == std::optional<std::string>("123456789A_"));
    }
}

TEST(FlashWorkflowTest, unisiaBootmodeWriteRunsKernelThenMod1ThenProgram)
{
    const auto paths = CatalogPaths();

    auto workflow = CreateWorkflow(UnisiaBootmodeWrite(paths));
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
    const auto paths = CatalogPaths();

    auto workflow = UnisiaBootmodeAtKernelAttempt(paths);
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
    const auto paths = CatalogPaths();

    auto workflow = UnisiaBootmodeAtKernelAttempt(paths);
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
    const auto paths = CatalogPaths();

    WorkflowTestPorts test_ports;
    auto workflow = CreateWorkflow(UnisiaBootmodeWrite(paths), test_ports);
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kApplyBootModeVoltages);
    workflow->Submit(FlashPromptResponse::kAccept);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(TransportKind::kKline));
    workflow->Submit(FlashAttemptResult{.success = true});
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kRemoveMod1);
    workflow->Submit(FlashPromptResponse::kDecline);
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(TransportKind::kKline));
    const auto notice = std::get<FlashPromptStep>(workflow->Next());
    ASSERT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    ASSERT_TRUE(notice.arguments == BootmodeNotice("cancelled"));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(TransportKind::kKline));
}

TEST(FlashWorkflowTest, unisiaBootmodeProgramFailureShowsNoticeThenFailure)
{
    const auto paths = CatalogPaths();

    auto workflow = UnisiaBootmodeAtKernelAttempt(paths);
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
    const auto paths = CatalogPaths();

    auto workflow = CreateWorkflow(UnisiaBootmodeWrite(paths));
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
    const auto paths = CatalogPaths();

    auto input = UnisiaBootmodeWrite(paths);
    input.image = bytes::Bytes(0x20001, 0xa5);
    auto workflow = CreateWorkflow(std::move(input));
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
}

TEST(FlashWorkflowTest, unisiaBootmodeMissingKernelFailsBeforeAnyPrompt)
{
    const auto paths = CatalogPaths();

    WorkflowTestPorts test_ports(false);
    auto workflow = CreateWorkflow(UnisiaBootmodeWrite(paths), test_ports);
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_TRUE(std::get<FlashFailureStep>(step).error.detail.find("catalog_uj20_bootmode.bin") != std::string::npos);
}

TEST(FlashWorkflowTest, unisiaBootmodeEmptyKernelFailsBeforeAnyPrompt)
{
    const auto paths = CatalogPaths();

    WorkflowTestPorts test_ports;
    test_ports.files->files[config::KernelFileHandle(paths, "catalog_uj20_bootmode.bin")] = {};
    auto workflow = CreateWorkflow(UnisiaBootmodeWrite(paths), test_ports);
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
}

TEST(FlashWorkflowTest, unisiaBootmodeTestWriteIsUnsupported)
{
    const auto paths = CatalogPaths();

    auto input = UnisiaBootmodeWrite(paths);
    input.operation = FlashOperation::kTestWrite;
    auto workflow = CreateWorkflow(std::move(input));
    const auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    ASSERT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kUnsupported);
}

TEST(FlashWorkflowTest, unisiaJecsM32rWriteWithoutAdapterVppPromptsBeforeAndAfter)
{
    // The default scripted source supplies no programming voltage, so the workflow prompts.
    auto workflow = CreateWorkflow(UnisiaM32rWrite());
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
    auto workflow = CreateWorkflow(UnisiaM32rWrite());
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
    WorkflowTestPorts test_ports(true, true);
    auto input = UnisiaM32rWrite();
    auto workflow = CreateWorkflow(std::move(input), test_ports);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kAccept);
    auto step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step));
    ASSERT_THAT(std::get<FlashAttempt>(step).attempt->Plan().Confirmations(), IsEmpty());
    workflow->Submit(FlashAttemptResult{.success = true});
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kSucceeded);
}

// OpenPort 2.0 supplies VPP, so no remove-VPP sentence is due; legacy still
// warned on every failed write not to power off the ECU.
std::unique_ptr<FlashWorkflow> UnisiaM32rAdapterVppWriteAtAttempt()
{
    WorkflowTestPorts test_ports(true, true);
    auto input = UnisiaM32rWrite();
    auto workflow = CreateWorkflow(std::move(input), test_ports);
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
    auto workflow = UnisiaM32rAdapterVppWriteAtAttempt();
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::kTimeout, .error_detail = "x"});
    const auto notice_step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(notice_step));
    const auto& notice = std::get<FlashPromptStep>(notice_step);
    ASSERT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    EXPECT_THAT(notice.arguments, ::testing::ElementsAre(std::pair<std::string, std::string>{"outcome", "failed"},
                                                         std::pair<std::string, std::string>{"external_vpp", "no"}));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto failure = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(failure));
    ASSERT_EQ(std::get<FlashFailureStep>(failure).error.kind, ErrorKind::kTimeout);
}

TEST(FlashWorkflowTest, unisiaJecsM32rAdapterSuppliedVppCancelledWriteWarnsNotToPowerOff)
{
    auto workflow = UnisiaM32rAdapterVppWriteAtAttempt();
    ASSERT_TRUE(workflow != nullptr);
    workflow->Submit(FlashAttemptResult{.success = false, .error_kind = ErrorKind::kCancelled});
    const auto notice_step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashPromptStep>(notice_step));
    const auto& notice = std::get<FlashPromptStep>(notice_step);
    ASSERT_EQ(notice.kind, FlashPromptKind::kRemoveProgrammingVoltage);
    EXPECT_THAT(notice.arguments, ::testing::ElementsAre(std::pair<std::string, std::string>{"outcome", "cancelled"},
                                                         std::pair<std::string, std::string>{"external_vpp", "no"}));
    workflow->Submit(FlashPromptResponse::kAccept);
    const auto done = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(done));
    ASSERT_EQ(std::get<FlashCompletedStep>(done).outcome, FlashWorkflowOutcome::kCancelled);
}

TEST(FlashWorkflowTest, unisiaJecsM32rReadPropagatesRomIdWithoutVppPrompts)
{
    auto input = Request("sub_ecu_unisia_jecs_30");
    input.protocol.mcu = "M32R_256KB";
    auto workflow = CreateWorkflow(std::move(input));
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
    auto workflow = CreateWorkflow(std::move(input));
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
    auto workflow = CreateWorkflow(std::move(input));
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

KernelImage CatalogKernelImage(std::string_view protocol, std::uint32_t load_address, bytes::Bytes bytes)
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
         CatalogKernelImage("sub_ecu_denso_sh7055_densocan", 0xFFFF6004, {0xaa, 0xbb, 0xcc, 0xdd}),
         {FlashPromptKind::kCycleIgnition}},
        {"sub_tcu_denso_sh7055_can",
         "SH7055",
         kSubaruTcuDensoSh705xCan,
         kCanIso15765,
         CatalogKernelImage("sub_tcu_denso_sh7055_can", 0xFFFF9000, {0x10, 0x20, 0x30, 0x40}),
         {}},
        {"sub_ecu_denso_sh7058_can",
         "SH7058",
         kSubaruDensoSh7058Can,
         kCanIso15765,
         CatalogKernelImage("sub_ecu_denso_sh7058_can", 0xFFFF3000, {0x90, 0xa0, 0xb0, 0xc0}),
         {}},
        {"sub_ecu_denso_sh7058_can_diesel",
         "SH7058d",
         kSubaruDensoSh7058CanDiesel,
         kCanIso15765,
         CatalogKernelImage("sub_ecu_denso_sh7058_can_diesel", 0xFFFF4000, {0xd0, 0xe0, 0xf0, 0x01}),
         {}},
        {"sub_ecu_denso_sh7055_04",
         "SH7055",
         kSubaruDensoSh705xKline,
         kKline,
         CatalogKernelImage("sub_ecu_denso_sh7055_04", 0xFFFF6004, {0xaa, 0xbb, 0xcc, 0xdd}),
         {}},
        {"sub_ecu_denso_sh7055_02",
         "SH7055",
         kSubaruDensoSh705502,
         kKline,
         CatalogKernelImage("sub_ecu_denso_sh7055_02", 0xFFFF6004, {0xaa, 0xbb, 0xcc, 0xdd}),
         {FlashPromptKind::kCycleIgnition}},
        {"sub_ecu_denso_mc68hc16y5_02",
         "MC68HC16Y5",
         kSubaruDensoMc68hc16y502,
         kKline,
         CatalogKernelImage("sub_ecu_denso_mc68hc16y5_02", 0x20000, {0x11, 0x22, 0x33}),
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
    const auto paths = CatalogPaths();

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);
        auto workflow = CreateWorkflow(SingleAttemptRead(test, paths));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = AcceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
        EXPECT_THAT(PromptKinds(prompts), ::testing::ElementsAreArray(WithBegin(test.confirmations)));
        EXPECT_THAT(prompts, ::testing::Each(::testing::Field(&FlashPromptStep::arguments, ::testing::SizeIs(0))));

        auto& attempt = std::get<FlashAttempt>(step);
        ASSERT_TRUE(attempt.attempt != nullptr);
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
        FakeClock clock;
        EXPECT_THAT(attempt.attempt->Run(clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::HasSubstr("before configure")));

        // Exactly one attempt: neither the pending nor the finished workflow
        // hands out a second one.
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->Next()));
        workflow->Submit(FlashAttemptResult{.success = true});
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->Next()));
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->Next()));
    }
}

TEST(FlashWorkflowTest, singleAttemptFamiliesAskTheSourceForTheirPlansTransport)
{
    const auto paths = CatalogPaths();

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);
        WorkflowTestPorts test_ports;
        auto workflow = CreateWorkflow(SingleAttemptRead(test, paths), test_ports);
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        const auto step = AcceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
        EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(test.transport));
    }
}

TEST(FlashWorkflowTest, unisiaBootmodeWriteMakesOneKlineTransportPerAttempt)
{
    const auto paths = CatalogPaths();

    WorkflowTestPorts test_ports;
    auto workflow = CreateWorkflow(UnisiaBootmodeWrite(paths), test_ports);
    ASSERT_TRUE(workflow != nullptr);
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::SizeIs(0));

    std::vector<FlashPromptStep> prompts;
    auto step = AcceptEveryPrompt(*workflow, prompts);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(TransportKind::kKline));
    workflow->Submit(FlashAttemptResult{.success = true});

    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kRemoveMod1);
    EXPECT_THAT(test_ports.transports->RequestedKinds(), ::testing::ElementsAre(TransportKind::kKline));
    workflow->Submit(FlashPromptResponse::kAccept);
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
    EXPECT_THAT(test_ports.transports->RequestedKinds(),
                ::testing::ElementsAre(TransportKind::kKline, TransportKind::kKline));
}

TEST(FlashWorkflowTest, singleAttemptFamiliesCancelWhenAnyPromptIsDeclined)
{
    const auto paths = CatalogPaths();

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(::testing::PrintToString(test));
        const std::vector<FlashPromptKind> sequence = WithBegin(test.confirmations);
        for (std::size_t declined = 0; declined < sequence.size(); ++declined)
        {
            SCOPED_TRACE(std::format("{} declining prompt {}", test.protocol, declined));
            auto workflow = CreateWorkflow(SingleAttemptRead(test, paths));
            ASSERT_TRUE(workflow != nullptr);
            for (std::size_t accepted = 0; accepted < declined; ++accepted)
            {
                SCOPED_TRACE(::testing::PrintToString(accepted));
                ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, sequence[accepted]);
                workflow->Submit(FlashPromptResponse::kAccept);
            }
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, sequence[declined]);
            workflow->Submit(FlashPromptResponse::kDecline);

            for (int repeat = 0; repeat < 2; ++repeat)
            {
                SCOPED_TRACE(::testing::PrintToString(repeat));
                const auto step = workflow->Next();
                ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
                const auto& done = std::get<FlashCompletedStep>(step);
                EXPECT_EQ(done.outcome, FlashWorkflowOutcome::kCancelled);
                EXPECT_THAT(done.accepted_read_bytes, Eq(std::nullopt));
                EXPECT_THAT(done.rom_id, Eq(std::nullopt));
            }
        }
    }
}

TEST(FlashWorkflowTest, singleAttemptFamiliesTreatEveryNonAcceptResponseAsDecline)
{
    const auto paths = CatalogPaths();

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(::testing::PrintToString(test));
        for (const auto response : {FlashPromptResponse::kSave, FlashPromptResponse::kDiscard})
        {
            SCOPED_TRACE(std::format("{} response {}", test.protocol, static_cast<int>(response)));
            auto workflow = CreateWorkflow(SingleAttemptRead(test, paths));
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
    auto workflow = CreateWorkflow(SingleAttemptRead(test, paths));
    std::vector<FlashPromptStep> prompts;
    if (workflow == nullptr || !std::holds_alternative<FlashAttempt>(AcceptEveryPrompt(*workflow, prompts)))
    {
        return nullptr;
    }
    return workflow;
}

TEST(FlashWorkflowTest, singleAttemptFamiliesReportEveryAttemptOutcome)
{
    const auto paths = CatalogPaths();

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);

        auto with_identity = SingleAttemptAtAttempt(test, paths);
        ASSERT_TRUE(with_identity != nullptr);
        with_identity->Submit(
            FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x5a, 0xa5}, .rom_id = "CAL_123456789A_"});
        auto step = with_identity->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kSucceeded);
        EXPECT_EQ(std::get<FlashCompletedStep>(step).accepted_read_bytes, bytes::Bytes({0x5a, 0xa5}));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).rom_id, std::string("CAL_123456789A_"));

        auto without_identity = SingleAttemptAtAttempt(test, paths);
        ASSERT_TRUE(without_identity != nullptr);
        without_identity->Submit(FlashAttemptResult{.success = true, .read_bytes = bytes::Bytes{0x01}});
        step = without_identity->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kSucceeded);
        EXPECT_EQ(std::get<FlashCompletedStep>(step).accepted_read_bytes, bytes::Bytes({0x01}));
        EXPECT_THAT(std::get<FlashCompletedStep>(step).rom_id, Eq(std::nullopt));

        auto failed = SingleAttemptAtAttempt(test, paths);
        ASSERT_TRUE(failed != nullptr);
        failed->Submit(FlashAttemptResult{.success = false,
                                          .error_kind = ErrorKind::kTimeout,
                                          .error_detail = "no reply to 0x34",
                                          .read_bytes = bytes::Bytes{0x02},
                                          .rom_id = "IGNORED"});
        step = failed->Next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::kTimeout, "no reply to 0x34"}));

        auto cancelled = SingleAttemptAtAttempt(test, paths);
        ASSERT_TRUE(cancelled != nullptr);
        cancelled->Submit(FlashAttemptResult{.success = false,
                                             .error_kind = ErrorKind::kCancelled,
                                             .error_detail = "cancelled: dialog closed",
                                             .read_bytes = bytes::Bytes{0x03}});
        step = cancelled->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kCancelled);
        EXPECT_THAT(std::get<FlashCompletedStep>(step).accepted_read_bytes, Eq(std::nullopt));
        EXPECT_THAT(std::get<FlashCompletedStep>(step).rom_id, Eq(std::nullopt));
    }
}

TEST(FlashWorkflowTest, singleAttemptFamiliesRejectInvalidConfigurationBeforeAnyPromptOrIo)
{
    const auto paths = CatalogPaths();

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(test.protocol);
        auto input = SingleAttemptRead(test, paths);
        input.protocol.mcu = "NOT_A_KNOWN_MCU";
        WorkflowTestPorts test_ports;
        PreflightTransportObservers observers(test_ports);
        auto workflow = CreateWorkflow(std::move(input), test_ports);
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            SCOPED_TRACE(::testing::PrintToString(repeat));
            const auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
        }
        observers.ExpectNoIo();
    }
}

TEST(FlashWorkflowTest, kernelBackedFamiliesFailBeforeAnyPromptOrIoWithoutTheirKernel)
{
    const auto catalog_only = CatalogPaths();

    for (const SingleAttemptCase& test : SingleAttemptCases())
    {
        SCOPED_TRACE(::testing::PrintToString(test));
        if (!test.kernel.has_value())
        {
            continue;
        }
        for (const bool has_catalog : {true, false})
        {
            SCOPED_TRACE(std::format("{} with{} catalog", test.protocol, has_catalog ? "" : "out"));
            auto input = SingleAttemptRead(test, has_catalog ? catalog_only : config::ConfigPaths{});
            WorkflowTestPorts test_ports(false);
            PreflightTransportObservers observers(test_ports);
            auto workflow = CreateWorkflow(std::move(input), test_ports);
            ASSERT_TRUE(workflow != nullptr);
            for (int repeat = 0; repeat < 2; ++repeat)
            {
                SCOPED_TRACE(::testing::PrintToString(repeat));
                const auto step = workflow->Next();
                ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
                EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
            }
            observers.ExpectNoIo();
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

        const auto paths = CatalogPaths();

        WorkflowTestPorts test_ports;
        auto input = SingleAttemptRead(test, paths);
        const auto handle = config::KernelFileHandle(paths, input.protocol.kernel);
        const auto expected_kernel_bytes = test.kernel->bytes;
        auto workflow = CreateWorkflow(std::move(input), test_ports);
        ASSERT_TRUE(workflow != nullptr);

        // Preflight snapshots the kernel before Begin; later steps reuse its bytes.
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
        test_ports.files->files.erase(handle);
        workflow->Submit(FlashPromptResponse::kAccept);

        std::vector<FlashPromptStep> prompts;
        auto step = AcceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
        EXPECT_THAT(PromptKinds(prompts), ::testing::ElementsAreArray(test.confirmations));
        const auto& attempt = std::get<FlashAttempt>(step);
        const auto& kernel = attempt.attempt->Plan().Kernel();
        ASSERT_THAT(kernel, Optional(_));
        EXPECT_EQ(kernel->id, test.kernel->id);
        EXPECT_EQ(kernel->load_address, test.kernel->load_address);
        EXPECT_EQ(test_ports.files->ReadCount(handle), 1);
        EXPECT_EQ(attempt.attempt->Plan().Kernel()->bytes, expected_kernel_bytes);
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
    auto workflow = CreateWorkflow(Sh7058Write());
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
    EXPECT_THAT(plan.Confirmations(), IsEmpty());

    // The K-Line executor's transport_setup() rejects a Write plan as
    // Unsupported, so reaching the pre-configure cancellation proves the CAN
    // executor is bound. Nothing is configured or opened on the way.
    FakeCancellationToken cancelled(true);
    NullEventSink events;
    FakeClock clock;
    EXPECT_THAT(attempt.attempt->Run(clock, cancelled, events),
                fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::HasSubstr("before configure")));

    workflow->Submit(
        FlashAttemptResult{.success = false, .error_kind = ErrorKind::kTimeout, .error_detail = "no reply to 0x34"});
    step = workflow->Next();
    ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
    EXPECT_EQ(std::get<FlashFailureStep>(step).error, (Error{ErrorKind::kTimeout, "no reply to 0x34"}));
}

TEST(FlashWorkflowTest, sh7058WriteCancelsWhenBeginIsDeclined)
{
    auto workflow = CreateWorkflow(Sh7058Write());
    ASSERT_TRUE(workflow != nullptr);
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    workflow->Submit(FlashPromptResponse::kDecline);
    for (int repeat = 0; repeat < 2; ++repeat)
    {
        SCOPED_TRACE(::testing::PrintToString(repeat));
        const auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kCancelled);
    }
}

TEST(FlashWorkflowTest, sh7058WriteRejectsInvalidInputBeforeAnyPromptOrIo)
{
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
        WorkflowTestPorts test_ports;
        PreflightTransportObservers observers(test_ports);
        auto workflow = CreateWorkflow(std::move(input), test_ports);
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            SCOPED_TRACE(::testing::PrintToString(repeat));
            const auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, test.expected);
        }
        observers.ExpectNoIo();
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
    const auto paths = CatalogPaths();

    for (const bool succeeded : {true, false})
    {
        SCOPED_TRACE(succeeded ? "succeeded" : "failed");
        auto workflow = CreateWorkflow(BdmWrite(paths));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = AcceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
        EXPECT_THAT(PromptKinds(prompts),
                    ::testing::ElementsAre(FlashPromptKind::kBegin, FlashPromptKind::kConfirmBdmKernelBootstrap));
        EXPECT_THAT(prompts, ::testing::Each(::testing::Field(&FlashPromptStep::arguments, ::testing::SizeIs(0))));

        auto& attempt = std::get<FlashAttempt>(step);
        const FlashPlan& plan = attempt.attempt->Plan();
        EXPECT_EQ(plan.Family(), FlashFamily::kSubaruDensoMc68hc16y502Bdm);
        EXPECT_EQ(plan.Transport(), TransportKind::kKline);
        EXPECT_EQ(plan.Operation(), FlashOperation::kWrite);
        EXPECT_EQ(plan.Image(), std::optional<bytes::Bytes>(BdmCatalogKernelImage()));
        EXPECT_EQ(plan.TransferRegion(), (MemoryRegion{0x20000, 0x20}));
        EXPECT_THAT(plan.Kernel(), Eq(std::nullopt));

        // transport_setup() validates the plan, so reaching the pre-configure
        // cancellation proves the BDM executor owns it.
        FakeCancellationToken cancelled(true);
        NullEventSink events;
        FakeClock clock;
        EXPECT_THAT(attempt.attempt->Run(clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::HasSubstr("before configure")));
        EXPECT_FALSE(std::holds_alternative<FlashAttempt>(workflow->Next()));

        if (succeeded)
        {
            workflow->Submit(FlashAttemptResult{.success = true});
            step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kSucceeded);
            EXPECT_THAT(std::get<FlashCompletedStep>(step).accepted_read_bytes, Eq(std::nullopt));
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
    const auto paths = CatalogPaths();

    const std::vector<FlashPromptKind> sequence{FlashPromptKind::kBegin, FlashPromptKind::kConfirmBdmKernelBootstrap};

    for (std::size_t declined = 0; declined < sequence.size(); ++declined)
    {
        SCOPED_TRACE(std::format("declining prompt {}", declined));
        auto workflow = CreateWorkflow(BdmWrite(paths));
        ASSERT_TRUE(workflow != nullptr);
        for (std::size_t accepted = 0; accepted < declined; ++accepted)
        {
            SCOPED_TRACE(::testing::PrintToString(accepted));
            ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, sequence[accepted]);
            workflow->Submit(FlashPromptResponse::kAccept);
        }
        ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, sequence[declined]);
        workflow->Submit(FlashPromptResponse::kDecline);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            SCOPED_TRACE(::testing::PrintToString(repeat));
            const auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
            EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kCancelled);
        }
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteWithoutItsKernelFailsBeforeAnyPromptOrIo)
{
    const auto catalog_only = CatalogPaths();

    for (const bool has_catalog : {true, false})
    {
        SCOPED_TRACE(has_catalog ? "with catalog" : "without catalog");
        auto input = BdmWrite(has_catalog ? catalog_only : config::ConfigPaths{});
        WorkflowTestPorts test_ports(false);
        PreflightTransportObservers observers(test_ports);
        auto workflow = CreateWorkflow(std::move(input), test_ports);
        ASSERT_TRUE(workflow != nullptr);
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            SCOPED_TRACE(::testing::PrintToString(repeat));
            const auto step = workflow->Next();
            ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
            EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
        }
        observers.ExpectNoIo();
    }
}

TEST(FlashWorkflowTest, mc68BdmWriteKeepsItsKernelSnapshotAfterFilesAreRemoved)
{
    const auto paths = CatalogPaths();

    WorkflowTestPorts test_ports;
    const auto handle = config::KernelFileHandle(paths, "catalog_mc68.bin");
    auto workflow = CreateWorkflow(BdmWrite(paths), test_ports);
    ASSERT_TRUE(workflow != nullptr);

    // Preflight snapshots the kernel before Begin; later steps reuse its bytes.
    ASSERT_EQ(std::get<FlashPromptStep>(workflow->Next()).kind, FlashPromptKind::kBegin);
    test_ports.files->files.erase(handle);
    workflow->Submit(FlashPromptResponse::kAccept);

    std::vector<FlashPromptStep> prompts;
    auto step = AcceptEveryPrompt(*workflow, prompts);
    ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
    EXPECT_THAT(PromptKinds(prompts), ::testing::ElementsAre(FlashPromptKind::kConfirmBdmKernelBootstrap));
    EXPECT_EQ(std::get<FlashAttempt>(step).attempt->Plan().Image(),
              std::optional<bytes::Bytes>(BdmCatalogKernelImage()));
    EXPECT_EQ(test_ports.files->ReadCount(handle), 1);
    EXPECT_THAT(std::get<FlashAttempt>(step).attempt->Plan().Kernel(), Eq(std::nullopt));
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
        auto workflow = CreateWorkflow(ColtWrite(test));
        ASSERT_TRUE(workflow != nullptr);

        std::vector<FlashPromptStep> prompts;
        auto step = AcceptEveryPrompt(*workflow, prompts);
        ASSERT_TRUE(std::holds_alternative<FlashAttempt>(step)) << FailureDetail(step);
        ASSERT_EQ(prompts.size(), test.prompts.size());
        for (std::size_t index = 0; index < prompts.size(); ++index)
        {
            SCOPED_TRACE(::testing::PrintToString(index));
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
        FakeClock clock;
        EXPECT_THAT(attempt.attempt->Run(clock, cancelled, events),
                    fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::HasSubstr("before configure")));

        workflow->Submit(FlashAttemptResult{.success = true});
        step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
        EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kSucceeded);
        EXPECT_THAT(std::get<FlashCompletedStep>(step).accepted_read_bytes, Eq(std::nullopt));
        EXPECT_THAT(std::get<FlashCompletedStep>(step).rom_id, Eq(std::nullopt));
    }
}

TEST(FlashWorkflowTest, coltWriteCancelsWhenAnyPromptIsDeclined)
{
    for (const ColtWriteCase& test : ColtWriteCases())
    {
        SCOPED_TRACE(::testing::PrintToString(test));
        for (std::size_t declined = 0; declined < test.prompts.size(); ++declined)
        {
            SCOPED_TRACE(std::format("{} declining prompt {}", test.protocol, declined));
            auto workflow = CreateWorkflow(ColtWrite(test));
            ASSERT_TRUE(workflow != nullptr);
            for (std::size_t accepted = 0; accepted < declined; ++accepted)
            {
                SCOPED_TRACE(::testing::PrintToString(accepted));
                ASSERT_THAT(std::get<FlashPromptStep>(workflow->Next()), IsPrompt(test.prompts[accepted]));
                workflow->Submit(FlashPromptResponse::kAccept);
            }
            ASSERT_THAT(std::get<FlashPromptStep>(workflow->Next()), IsPrompt(test.prompts[declined]));
            workflow->Submit(FlashPromptResponse::kDecline);
            for (int repeat = 0; repeat < 2; ++repeat)
            {
                SCOPED_TRACE(::testing::PrintToString(repeat));
                const auto step = workflow->Next();
                ASSERT_TRUE(std::holds_alternative<FlashCompletedStep>(step));
                EXPECT_EQ(std::get<FlashCompletedStep>(step).outcome, FlashWorkflowOutcome::kCancelled);
            }
        }
    }
}

TEST(FlashWorkflowTest, coltWriteWithWrongImageSizeFailsBeforeAnyPromptOrIo)
{
    for (const ColtWriteCase& test : ColtWriteCases())
    {
        SCOPED_TRACE(test.protocol);
        auto input = ColtWrite(test);
        input.image = bytes::Bytes(test.capacity + 1, 0x5a);
        WorkflowTestPorts test_ports;
        PreflightTransportObservers observers(test_ports);
        auto workflow = CreateWorkflow(std::move(input), test_ports);
        ASSERT_TRUE(workflow != nullptr);
        const auto step = workflow->Next();
        ASSERT_TRUE(std::holds_alternative<FlashFailureStep>(step));
        EXPECT_EQ(std::get<FlashFailureStep>(step).error.kind, ErrorKind::kInvalidConfig);
        observers.ExpectNoIo();
    }
}

} // namespace
} // namespace fastecu::flash
