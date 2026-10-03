#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <format>
#include <limits>
#include <string_view>

#include "src/backend/config/protocol_catalog.h"
#include "src/backend/definition/text_format.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_kline_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_executor.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.h"
#include "src/backend/flash/eeprom/eeprom_read_plan.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_kline_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h"

namespace fastecu::flash
{
namespace
{

FlashCompletedStep completed(FlashWorkflowOutcome outcome, std::optional<bytes::Bytes> bytes = std::nullopt,
                             std::optional<std::string> rom_id = std::nullopt)
{
    return {outcome, std::move(bytes), std::move(rom_id)};
}

// Every FlashWorkflow ends in exactly one of these four outcomes, reached
// either directly from a FlashAttemptResult (record()) or from a workflow's
// own staging logic (succeed()/cancel()/discard()/fail()). Owning the state
// here, instead of in each workflow, keeps the workflows below from repeating
// the same five fields and the same result-handling branches.
class FlashAttemptOutcome
{
  public:
    void succeed(std::optional<bytes::Bytes> bytes = std::nullopt, std::optional<std::string> rom_id = std::nullopt)
    {
        terminal_ = true;
        outcome_ = FlashWorkflowOutcome::Succeeded;
        bytes_ = std::move(bytes);
        rom_id_ = std::move(rom_id);
    }
    void cancel()
    {
        terminal_ = true;
        outcome_ = FlashWorkflowOutcome::Cancelled;
    }
    void discard()
    {
        terminal_ = true;
        outcome_ = FlashWorkflowOutcome::Discarded;
    }
    void fail(Error error)
    {
        terminal_ = true;
        outcome_ = FlashWorkflowOutcome::Failed;
        failure_ = std::move(error);
    }

    // The success/cancelled/failed mapping shared by every workflow whose
    // attempt result finalizes the outcome directly.
    void record(FlashAttemptResult result)
    {
        if (result.success)
        {
            succeed(std::move(result.read_bytes), std::move(result.rom_id));
        }
        else if (result.error_kind == ErrorKind::Cancelled)
        {
            cancel();
        }
        else
        {
            fail(Error{result.error_kind, std::move(result.error_detail)});
        }
    }

    bool terminal() const
    {
        return terminal_;
    }
    // The recorded failure, moved out; empty if the workflow has not failed.
    std::optional<FlashFailureStep> takeFailure()
    {
        if (!failure_.has_value())
        {
            return std::nullopt;
        }
        return FlashFailureStep{std::move(*failure_)};
    }
    FlashCompletedStep completedStep()
    {
        return completed(outcome_, std::move(bytes_), std::move(rom_id_));
    }

  private:
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::Failed;
    std::optional<bytes::Bytes> bytes_;
    std::optional<std::string> rom_id_;
    std::optional<Error> failure_;
};

Result<std::uint32_t> parseKernelStartAddress(std::string_view kernel_addr)
{
    const auto parsed = definition::parse_hex_value(kernel_addr);
    if (!parsed.has_value() || *parsed > std::numeric_limits<std::uint32_t>::max())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("kernel_addr did not parse as a 32-bit address: '{}'", kernel_addr));
    }
    return static_cast<std::uint32_t>(*parsed);
}

Result<config::ProtocolEntry> resolveProtocol(const config::ConfigPaths& paths, std::string_view protocol_name,
                                              IFileRepository& repository)
{
    Result<config::ProtocolCatalog> protocols = config::load_protocol_catalog(paths, repository);
    if (!protocols.has_value())
    {
        return std::unexpected(protocols.error());
    }
    const auto entry = std::ranges::find(*protocols, protocol_name, &config::ProtocolEntry::protocol_name);
    if (entry == protocols->end())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("protocol '{}' is absent from the <protocols> section", protocol_name));
    }
    return *entry;
}

Result<KernelImage> resolveKernel(const FlashWorkflowRequest& request, IFileRepository& repository)
{
    Result<config::ProtocolEntry> entry = resolveProtocol(request.paths, request.protocol, repository);
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    Result<std::uint32_t> load_address = parseKernelStartAddress(entry->kernel_addr);
    if (!load_address.has_value())
    {
        return std::unexpected(load_address.error());
    }
    Result<std::vector<std::uint8_t>> kernel_bytes =
        repository.read(request.paths.kernel_files_directory + entry->kernel);
    if (!kernel_bytes.has_value())
    {
        return std::unexpected(kernel_bytes.error());
    }
    return KernelImage{
        .id = request.protocol + "-kernel", .load_address = *load_address, .bytes = std::move(*kernel_bytes)};
}

// The cfg <kernel> file alone. The Unisia Jecs M32R _bootmode entries declare
// no <kernel_addr>: the M32R boot ROM places the kernel itself.
Result<bytes::Bytes> resolveKernelBytes(const FlashWorkflowRequest& request, IFileRepository& repository)
{
    Result<config::ProtocolEntry> entry = resolveProtocol(request.paths, request.protocol, repository);
    if (!entry.has_value())
    {
        return std::unexpected(entry.error());
    }
    Result<std::vector<std::uint8_t>> kernel_bytes =
        repository.read(request.paths.kernel_files_directory + entry->kernel);
    if (!kernel_bytes.has_value())
    {
        const Error& error = kernel_bytes.error();
        return fail(error.kind, std::format("kernel file '{}': {}", entry->kernel, error.detail));
    }
    return bytes::Bytes(kernel_bytes->begin(), kernel_bytes->end());
}

std::optional<bytes::Bytes> normalizeMc68Image(std::optional<bytes::Bytes> image, std::string_view mcu_name)
{
    if (!image.has_value())
    {
        return std::nullopt;
    }
    const flashdev_t *device = find_flash_device(mcu_name);
    if (device == nullptr || image->size() == device->romsize)
    {
        return image;
    }

    std::size_t physical_size = 0;
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        physical_size = std::max(physical_size, static_cast<std::size_t>(block.start) + block.len);
    }
    if (image->size() != physical_size)
    {
        return image;
    }

    bytes::Bytes packed;
    packed.reserve(device->romsize);
    std::size_t packed_remaining = device->romsize;
    for (unsigned block_no = 0; block_no < device->numblocks && packed_remaining > 0; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        const std::size_t block_bytes = std::min<std::size_t>(block.len, packed_remaining);
        packed.append_range(bytes::ByteView(*image).subspan(block.start, block_bytes));
        packed_remaining -= block_bytes;
    }
    return packed;
}

// Wave 6c-3. The adapter check moved here from legacy write_mem() :434: an
// adapter that supplies programming voltage needs no operator prompt. With no
// serial at all the workflow cannot know, so it prompts.
class SubaruUnisiaJecsM32rKlineWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruUnisiaJecsM32rKlineWorkflow(FlashWorkflowRequest request)
        : request_(std::move(request)),
          plan_(build_subaru_unisia_jecs_m32r_kline_plan(request_.operation, request_.protocol, request_.mcu,
                                                         std::move(request_.image),
                                                         adapter_supplies_programming_voltage(request_.serial)))
    {
        if (plan_.has_value())
        {
            needs_vpp_ = std::ranges::any_of(plan_->confirmations(), [](const ConfirmationSpec& spec)
                                             { return spec.id == ConfirmationSpec::Id::ApplyProgrammingVoltage; });
        }
    }

    FlashWorkflowStep next() override
    {
        if (!plan_.has_value())
        {
            return FlashFailureStep{plan_.error()};
        }
        // The reminder precedes whatever the attempt produced, failure included.
        if (stage_ == Stage::RemoveVpp)
        {
            return FlashPromptStep{FlashPromptKind::RemoveProgrammingVoltage,
                                   {{"outcome", attempt_outcome_}, {"external_vpp", needs_vpp_ ? "yes" : "no"}}};
        }
        if (auto failure = outcome_.takeFailure(); failure.has_value())
        {
            return std::move(*failure);
        }
        if (outcome_.terminal())
        {
            return outcome_.completedStep();
        }
        if (stage_ == Stage::Begin)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (stage_ == Stage::ApplyVpp)
        {
            return FlashPromptStep{FlashPromptKind::ApplyProgrammingVoltage, {}};
        }
        if (stage_ == Stage::Attempt)
        {
            stage_ = Stage::Done;
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     bind_flash_attempt(std::move(*plan_),
                                                        std::make_unique<SubaruUnisiaJecsM32rKlineExecutor>(),
                                                        std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                                     std::make_unique<QtClock>()};
        }
        return outcome_.completedStep();
    }

    void submit(FlashPromptResponse response) override
    {
        switch (stage_)
        {
        case Stage::Begin:
        case Stage::ApplyVpp:
            if (response != FlashPromptResponse::Accept)
            {
                outcome_.cancel();
                return;
            }
            stage_ = stage_ == Stage::Begin && needs_vpp_ ? Stage::ApplyVpp : Stage::Attempt;
            return;
        case Stage::RemoveVpp:
            stage_ = Stage::Done; // OK-only notice
            return;
        case Stage::Attempt:
        case Stage::Done:
            return;
        }
    }

    void submit(FlashAttemptResult result) override
    {
        // Legacy warned after every failed write, whatever the adapter, not to
        // power off the ECU; the remove-VPP sentence is due only when the
        // operator applied external VPP, success included.
        if (is_write_ && (needs_vpp_ || !result.success))
        {
            attempt_outcome_ = result.success                              ? "succeeded"
                               : result.error_kind == ErrorKind::Cancelled ? "cancelled"
                                                                           : "failed";
            stage_ = Stage::RemoveVpp;
        }
        outcome_.record(std::move(result));
    }

  private:
    enum class Stage
    {
        Begin,
        ApplyVpp,
        Attempt,
        RemoveVpp,
        Done,
    };

    FlashWorkflowRequest request_;
    Result<FlashPlan> plan_;
    bool is_write_ = request_.operation == FlashOperation::Write;
    bool needs_vpp_ = false;
    Stage stage_ = Stage::Begin;
    std::string attempt_outcome_;
    FlashAttemptOutcome outcome_;
};

// Wave 7. Read is the 6c-3 K-Line read. Write is two attempts with an
// operator step between them, where legacy reset the connection anyway
// (flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp:361-367):
// kernel upload, RemoveMod1, erase and program. Both plans are built before
// Begin so a missing kernel or a wrong-size image fails before any prompt.
class SubaruUnisiaJecsM32rBootModeWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruUnisiaJecsM32rBootModeWorkflow(FlashWorkflowRequest request) : request_(std::move(request))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!built_.has_value())
        {
            built_ = buildPlans();
        }
        if (!built_->has_value())
        {
            return FlashFailureStep{built_->error()};
        }
        // The notice precedes whatever the attempt produced, failure included.
        if (stage_ == Stage::Notice)
        {
            return FlashPromptStep{FlashPromptKind::RemoveProgrammingVoltage,
                                   {{"outcome", notice_outcome_}, {"external_vpp", "yes"}, {"power_off_advice", "no"}}};
        }
        if (auto failure = outcome_.takeFailure(); failure.has_value())
        {
            return std::move(*failure);
        }
        if (outcome_.terminal())
        {
            return outcome_.completedStep();
        }
        switch (stage_)
        {
        case Stage::Begin:
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        case Stage::ApplyVoltages:
            return FlashPromptStep{FlashPromptKind::ApplyBootModeVoltages, {}};
        case Stage::FirstAttempt:
            stage_ = Stage::AwaitFirst;
            return firstAttempt();
        case Stage::RemoveMod1:
            return FlashPromptStep{FlashPromptKind::RemoveMod1, {}};
        case Stage::ProgramAttempt:
        {
            std::optional<FlashPlan>& program = (*built_)->program;
            if (!program.has_value())
            {
                return FlashFailureStep{Error{ErrorKind::Internal, "the Unisia JECS program plan was not built"}};
            }
            stage_ = Stage::AwaitProgram;
            return attempt(std::move(*program), std::make_unique<SubaruUnisiaJecsM32rBootModeProgramExecutor>());
        }
        case Stage::AwaitFirst:
        case Stage::AwaitProgram:
        case Stage::Notice:
        case Stage::Done:
            break;
        }
        return outcome_.completedStep();
    }

    void submit(FlashPromptResponse response) override
    {
        switch (stage_)
        {
        case Stage::Begin:
        case Stage::ApplyVoltages:
            if (response != FlashPromptResponse::Accept)
            {
                outcome_.cancel();
                return;
            }
            stage_ = stage_ == Stage::Begin && is_write() ? Stage::ApplyVoltages : Stage::FirstAttempt;
            return;
        case Stage::RemoveMod1:
            if (response != FlashPromptResponse::Accept)
            {
                // The kernel runs and nothing is erased; still ask for VPP removal.
                notice_outcome_ = "cancelled";
                stage_ = Stage::Notice;
                outcome_.cancel();
                return;
            }
            stage_ = Stage::ProgramAttempt;
            return;
        case Stage::Notice:
            stage_ = Stage::Done; // OK-only notice
            return;
        case Stage::FirstAttempt:
        case Stage::AwaitFirst:
        case Stage::ProgramAttempt:
        case Stage::AwaitProgram:
        case Stage::Done:
            return;
        }
    }

    void submit(FlashAttemptResult result) override
    {
        if (!is_write())
        {
            outcome_.record(std::move(result));
            return;
        }
        // A successful kernel upload is not an outcome yet: RemoveMod1 follows.
        if (stage_ == Stage::AwaitFirst && result.success)
        {
            stage_ = Stage::RemoveMod1;
            return;
        }
        notice_outcome_ = result.success                              ? "succeeded"
                          : result.error_kind == ErrorKind::Cancelled ? "cancelled"
                                                                      : "failed";
        stage_ = Stage::Notice;
        outcome_.record(std::move(result));
    }

  private:
    enum class Stage
    {
        Begin,
        ApplyVoltages,
        FirstAttempt,
        AwaitFirst,
        RemoveMod1,
        ProgramAttempt,
        AwaitProgram,
        Notice,
        Done,
    };

    struct Plans
    {
        std::optional<FlashPlan> first;   // Read plan, or the kernel upload
        std::optional<FlashPlan> program; // Write only
    };

    bool is_write() const
    {
        return request_.operation != FlashOperation::Read;
    }

    Result<Plans> buildPlans()
    {
        if (!is_write())
        {
            // adapter_supplies_programming_voltage is irrelevant to Read.
            auto read = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, request_.protocol, request_.mcu,
                                                                 std::nullopt, true);
            if (!read.has_value())
            {
                return std::unexpected(read.error());
            }
            return Plans{std::move(*read), std::nullopt};
        }
        // Program first: it needs no I/O and rejects TestWrite and a wrong
        // image before the kernel file is read.
        auto program = build_subaru_unisia_jecs_m32r_bootmode_program_plan(request_.operation, request_.protocol,
                                                                           request_.mcu, std::move(request_.image));
        if (!program.has_value())
        {
            return std::unexpected(program.error());
        }
        QtFileRepository repository;
        Result<bytes::Bytes> kernel_bytes = resolveKernelBytes(request_, repository);
        if (!kernel_bytes.has_value())
        {
            return std::unexpected(kernel_bytes.error());
        }
        auto kernel = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(request_.operation, request_.protocol,
                                                                         request_.mcu, std::move(*kernel_bytes));
        if (!kernel.has_value())
        {
            return std::unexpected(kernel.error());
        }
        return Plans{std::move(*kernel), std::move(*program)};
    }

    FlashWorkflowStep firstAttempt()
    {
        if (!built_.has_value() || !built_->has_value())
        {
            return FlashFailureStep{Error{ErrorKind::Internal, "the Unisia JECS plans were not built"}};
        }
        std::optional<FlashPlan>& first = (*built_)->first;
        if (!first.has_value())
        {
            return FlashFailureStep{Error{ErrorKind::Internal, "the Unisia JECS first-stage plan was not built"}};
        }
        FlashPlan plan = std::move(*first);
        if (!is_write())
        {
            return attempt(std::move(plan), std::make_unique<SubaruUnisiaJecsM32rKlineExecutor>());
        }
        return attempt(std::move(plan), std::make_unique<SubaruUnisiaJecsM32rBootModeKernelExecutor>());
    }

    template <typename Executor> FlashWorkflowStep attempt(FlashPlan plan, std::unique_ptr<Executor> executor)
    {
        return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                 bind_flash_attempt(std::move(plan), std::move(executor),
                                                    std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                                 std::make_unique<QtClock>()};
    }

    FlashWorkflowRequest request_;
    std::optional<Result<Plans>> built_;
    Stage stage_ = Stage::Begin;
    std::string notice_outcome_;
    FlashAttemptOutcome outcome_;
};

// The prompt that collects each confirmation a single-attempt plan can carry,
// arguments unchanged. The other ids belong to workflows with their own
// staging; meeting one here is a routing defect, reported before any prompt
// or hardware access.
Result<FlashPromptStep> confirmationPrompt(const ConfirmationSpec& confirmation)
{
    using enum ConfirmationSpec::Id;
    switch (confirmation.id)
    {
    case CycleIgnition:
        return FlashPromptStep{FlashPromptKind::CycleIgnition, confirmation.arguments};
    case EraseTrigger:
        return FlashPromptStep{FlashPromptKind::ColtEraseTrigger, confirmation.arguments};
    case TopRegionBootstrap:
        return FlashPromptStep{FlashPromptKind::ColtTopRegionBootstrap, confirmation.arguments};
    case StartKlineRead:
        return FlashPromptStep{FlashPromptKind::ConfirmSh7058Read, confirmation.arguments};
    case KernelBootstrap:
        return FlashPromptStep{FlashPromptKind::ConfirmBdmKernelBootstrap, confirmation.arguments};
    case BeginEepromRead:
    case InspectEepromBytes:
    case ApplyProgrammingVoltage:
    case ApplyBootModeVoltages:
        break;
    }
    return fail(ErrorKind::Internal,
                std::format("confirmation {} has no single-attempt prompt", static_cast<int>(confirmation.id)));
}

// Begin, then one prompt per plan confirmation, in plan order.
Result<std::vector<FlashPromptStep>> promptSequence(const FlashPlan& plan)
{
    std::vector<FlashPromptStep> prompts{FlashPromptStep{FlashPromptKind::Begin, {}}};
    for (const ConfirmationSpec& confirmation : plan.confirmations())
    {
        Result<FlashPromptStep> prompt = confirmationPrompt(confirmation);
        if (!prompt.has_value())
        {
            return std::unexpected(prompt.error());
        }
        prompts.push_back(std::move(*prompt));
    }
    return prompts;
}

using KernelFreePlanBuilder = Result<FlashPlan> (*)(FlashOperation, std::string_view, std::string_view,
                                                    std::optional<bytes::Bytes>);
using KernelBackedPlanBuilder = Result<FlashPlan> (*)(FlashOperation, std::string_view, std::string_view,
                                                      std::optional<bytes::Bytes>, KernelImage);

// Plan preparation for a kernel-free family: the builder needs no I/O, so the
// plan is built with the workflow.
template <KernelFreePlanBuilder Build> class EagerPlan
{
  public:
    explicit EagerPlan(FlashWorkflowRequest& request)
        : plan_(Build(request.operation, request.protocol, request.mcu, std::move(request.image)))
    {
    }

    Result<FlashPlan>& plan(const FlashWorkflowRequest&)
    {
        return plan_;
    }

  private:
    Result<FlashPlan> plan_;
};

using PlanPreparation = Result<FlashPlan> (*)(FlashWorkflowRequest&);

// Prepares the plan on the first next(), before Begin, and keeps the result:
// a preparation failure is reported before any prompt, and the attempt
// carries the snapshot taken then even if the files change afterward.
template <PlanPreparation Prepare> class LazyPlan
{
  public:
    explicit LazyPlan(const FlashWorkflowRequest&)
    {
    }

    Result<FlashPlan>& plan(FlashWorkflowRequest& request)
    {
        if (!plan_.has_value())
        {
            plan_ = Prepare(request);
        }
        return *plan_;
    }

  private:
    std::optional<Result<FlashPlan>> plan_;
};

// A kernel-backed family: the catalog and kernel file are read, then the
// family builder runs.
template <KernelBackedPlanBuilder Build> Result<FlashPlan> prepareKernelBacked(FlashWorkflowRequest& request)
{
    QtFileRepository repository;
    Result<KernelImage> kernel = resolveKernel(request, repository);
    if (!kernel.has_value())
    {
        return std::unexpected(kernel.error());
    }
    return Build(request.operation, request.protocol, request.mcu, std::move(request.image), std::move(*kernel));
}

// MC68HC16Y5_02: prepareKernelBacked with two family steps before the kernel
// is read.
Result<FlashPlan> prepareMc68(FlashWorkflowRequest& request)
{
    // Desktop FullRomData is physically addressed after the legacy
    // calibration adapter inserts the 0x20000-0x27fff RAM/kernel hole.
    // Portable MC plans and executors use the packed flash-block image.
    request.image = normalizeMc68Image(std::move(request.image), request.mcu);
    // Run the family builder first so recognized-but-unsupported
    // revision 04 is rejected by the plan even without a catalog.
    Result<FlashPlan> preflight = build_subaru_denso_mc68hc16y5_02_plan(
        request.operation, request.protocol, request.mcu, request.image,
        KernelImage{.id = request.protocol + "-kernel", .load_address = 0x20000, .bytes = {0}});
    if (!preflight.has_value())
    {
        return std::unexpected(preflight.error());
    }
    QtFileRepository repository;
    Result<KernelImage> kernel = resolveKernel(request, repository);
    if (!kernel.has_value())
    {
        return std::unexpected(kernel.error());
    }
    return build_subaru_denso_mc68hc16y5_02_plan(request.operation, request.protocol, request.mcu,
                                                 std::move(request.image), std::move(*kernel));
}

// MC68HC16Y5 BDM: only Write reads the catalog -- its "write" uploads and
// starts the cfg kernel. The operator's ROM (request.image) is never
// forwarded: BDM never writes the ROM.
Result<FlashPlan> prepareBdm(FlashWorkflowRequest& request)
{
    if (request.operation != FlashOperation::Write)
    {
        return build_subaru_denso_mc68hc16y5_02_bdm_plan(request.operation, request.protocol, request.mcu, std::nullopt,
                                                         std::nullopt);
    }
    QtFileRepository repository;
    Result<KernelImage> kernel = resolveKernel(request, repository);
    if (!kernel.has_value())
    {
        return std::unexpected(kernel.error());
    }
    return build_subaru_denso_mc68hc16y5_02_bdm_plan(request.operation, request.protocol, request.mcu, std::nullopt,
                                                     std::move(*kernel));
}

template <KernelBackedPlanBuilder Build> using CachedKernelPlan = LazyPlan<&prepareKernelBacked<Build>>;
using Mc68KernelPlan = LazyPlan<&prepareMc68>;
using BdmKernelPlan = LazyPlan<&prepareBdm>;

// The control flow shared by every family whose operation is one attempt:
// preflight, Begin, the plan's confirmations in order, the attempt, then its
// result. Accepting a prompt advances to the next one; any other response
// cancels before the attempt is made. Families differ only in the executor,
// the desktop transport bound to it (bind_flash_attempt rejects a mismatch at
// compile time), and how the plan is prepared.
template <typename Executor, typename Transport, typename Preparation>
class SingleAttemptFlashWorkflow final : public FlashWorkflow
{
  public:
    explicit SingleAttemptFlashWorkflow(FlashWorkflowRequest request)
        : request_(std::move(request)), preparation_(request_)
    {
    }

    FlashWorkflowStep next() override
    {
        Result<FlashPlan>& plan = preparation_.plan(request_);
        if (!plan.has_value())
        {
            return FlashFailureStep{plan.error()};
        }
        if (!prompts_.has_value())
        {
            prompts_ = promptSequence(*plan);
        }
        if (!prompts_->has_value())
        {
            return FlashFailureStep{prompts_->error()};
        }
        if (auto failure = outcome_.takeFailure(); failure.has_value())
        {
            return std::move(*failure);
        }
        if (outcome_.terminal())
        {
            return outcome_.completedStep();
        }
        if (const std::vector<FlashPromptStep>& prompts = **prompts_; accepted_ < prompts.size())
        {
            return prompts[accepted_];
        }
        if (!attempted_)
        {
            attempted_ = true;
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     bind_flash_attempt(std::move(*plan), std::make_unique<Executor>(),
                                                        std::make_unique<Transport>(request_.serial)),
                                     std::make_unique<QtClock>()};
        }
        return outcome_.completedStep();
    }

    void submit(FlashPromptResponse response) override
    {
        if (response != FlashPromptResponse::Accept)
        {
            outcome_.cancel();
            return;
        }
        ++accepted_;
    }

    void submit(FlashAttemptResult result) override
    {
        outcome_.record(std::move(result));
    }

  private:
    FlashWorkflowRequest request_;
    Preparation preparation_;
    std::optional<Result<std::vector<FlashPromptStep>>> prompts_;
    std::size_t accepted_ = 0;
    bool attempted_ = false;
    FlashAttemptOutcome outcome_;
};

template <typename Executor, typename Transport, KernelFreePlanBuilder Build>
using KernelFreeWorkflow = SingleAttemptFlashWorkflow<Executor, Transport, EagerPlan<Build>>;

template <typename Executor, KernelFreePlanBuilder Build>
using KernelFreeCanWorkflow = KernelFreeWorkflow<Executor, DesktopCanFlashTransport, Build>;

template <typename Executor, KernelFreePlanBuilder Build>
using KernelFreeKlineWorkflow = KernelFreeWorkflow<Executor, DesktopKlineFlashTransport, Build>;

template <typename Executor, typename Transport, KernelBackedPlanBuilder Build>
using KernelBackedWorkflow = SingleAttemptFlashWorkflow<Executor, Transport, CachedKernelPlan<Build>>;

// Colt write plans carry EraseTrigger, and the 512 KiB variants also
// TopRegionBootstrap; read plans carry neither.
using ColtWorkflow = KernelFreeCanWorkflow<MitsuColtM32rCanExecutor, &build_mitsu_colt_m32r_can_plan>;
using SubaruHitachiM32rCanWorkflow =
    KernelFreeCanWorkflow<SubaruHitachiM32rCanExecutor, &build_subaru_hitachi_m32r_can_plan>;
using SubaruTcuCvtHitachiM32rCanWorkflow =
    KernelFreeCanWorkflow<SubaruTcuCvtHitachiM32rCanExecutor, &build_subaru_tcu_cvt_hitachi_m32r_can_plan>;
using SubaruTcuCvtMitsuMh8111CanWorkflow =
    KernelFreeCanWorkflow<SubaruTcuCvtMitsuMh8111CanExecutor, &build_subaru_tcu_cvt_mitsu_mh8111_can_plan>;
using SubaruTcuCvtMitsuMh8104CanWorkflow =
    KernelFreeCanWorkflow<SubaruTcuCvtMitsuMh8104CanExecutor, &build_subaru_tcu_cvt_mitsu_mh8104_can_plan>;
using SubaruDenso1n83m_1_5mCanWorkflow =
    KernelFreeCanWorkflow<SubaruDenso1n83m_1_5mCanExecutor, &build_subaru_denso_1n83m_1_5m_can_plan>;
using SubaruDensoSh72531CanWorkflow =
    KernelFreeCanWorkflow<SubaruDensoSh72531CanExecutor, &build_subaru_denso_sh72531_can_plan>;
using SubaruDensoSh72543CanDieselWorkflow =
    KernelFreeCanWorkflow<SubaruDensoSh72543CanDieselExecutor, &build_subaru_denso_sh72543_can_diesel_plan>;
using SubaruDenso1n83m_4mCanWorkflow =
    KernelFreeCanWorkflow<SubaruDenso1n83m_4mCanExecutor, &build_subaru_denso_1n83m_4m_can_plan>;
// Supports Read and Write. TestWrite is rejected by the plan builder: the
// legacy reflash_block ignored its test_write_arg and erased and wrote for
// real, so there is no dry run to route to.
using SubaruTcuHitachiM32rCanWorkflow =
    KernelFreeCanWorkflow<SubaruTcuHitachiM32rCanExecutor, &build_subaru_tcu_hitachi_m32r_can_plan>;
using SubaruHitachiSh72543rCanWorkflow =
    KernelFreeCanWorkflow<SubaruHitachiSh72543rCanExecutor, &build_subaru_hitachi_sh72543r_can_plan>;

using SubaruMitsuM32rKlineWorkflow =
    KernelFreeKlineWorkflow<SubaruMitsuM32rKlineExecutor, &build_subaru_mitsu_m32r_kline_plan>;
using SubaruHitachiM32rKlineWorkflow =
    KernelFreeKlineWorkflow<SubaruHitachiM32rKlineExecutor, &build_subaru_hitachi_m32r_kline_plan>;
using SubaruTcuHitachiM32rKlineWorkflow =
    KernelFreeKlineWorkflow<SubaruTcuHitachiM32rKlineExecutor, &build_subaru_tcu_hitachi_m32r_kline_plan>;
using SubaruUnisiaJecsWorkflow = KernelFreeKlineWorkflow<SubaruUnisiaJecsExecutor, &build_subaru_unisia_jecs_plan>;

// DensoCAN plans carry exactly one CycleIgnition confirmation; the other
// kernel-backed plans carry none.
using SubaruDensoSh705xDensoCanWorkflow =
    KernelBackedWorkflow<SubaruDensoSh705xDensoCanExecutor, DesktopMixedCanFlashTransport,
                         &build_subaru_denso_sh705x_densocan_plan>;
using SubaruTcuDensoSh705xCanWorkflow = KernelBackedWorkflow<SubaruTcuDensoSh705xCanExecutor, DesktopCanFlashTransport,
                                                             &build_subaru_tcu_denso_sh705x_can_plan>;
using SubaruDensoSh7058CanWorkflow =
    KernelBackedWorkflow<SubaruDensoSh7058CanExecutor, DesktopCanFlashTransport, &build_subaru_denso_sh7058_can_plan>;
using SubaruDensoSh7058CanDieselWorkflow =
    KernelBackedWorkflow<SubaruDensoSh7058CanDieselExecutor, DesktopCanFlashTransport,
                         &build_subaru_denso_sh7058_can_diesel_plan>;
// Wave 6b-2. The sequence is the kernel resolved on the first step, the shared
// Begin prompt -- the legacy dialog's only prompt, "Turn ignition ON" -- then
// the attempt.
using SubaruDensoSh705xKlineWorkflow = KernelBackedWorkflow<SubaruDensoSh705xKlineExecutor, DesktopKlineFlashTransport,
                                                            &build_subaru_denso_sh705x_kline_plan>;
// SH7055_02 plans carry exactly one CycleIgnition confirmation.
using SubaruDensoSh7055_02Workflow =
    KernelBackedWorkflow<SubaruDensoSh7055_02Executor, DesktopKlineFlashTransport, &build_subaru_denso_sh7055_02_plan>;
using SubaruDensoMc68hc16y5_02Workflow =
    SingleAttemptFlashWorkflow<SubaruDensoMc68hc16y5_02Executor, DesktopKlineFlashTransport, Mc68KernelPlan>;
// Hitachi SH7058 Read runs over K-Line and Write over CAN: the plan builder
// chooses the transport by operation and the factory the matching executor.
// Read plans carry StartKlineRead; write plans carry nothing.
using SubaruHitachiSh7058KlineWorkflow =
    KernelFreeKlineWorkflow<SubaruHitachiSh7058KlineExecutor, &build_subaru_hitachi_sh7058_plan>;
using SubaruHitachiSh7058CanWorkflow =
    KernelFreeCanWorkflow<SubaruHitachiSh7058CanExecutor, &build_subaru_hitachi_sh7058_plan>;
// Write plans carry KernelBootstrap; read plans carry nothing.
using SubaruDensoMc68hc16y5_02BdmWorkflow =
    SingleAttemptFlashWorkflow<SubaruDensoMc68hc16y5_02BdmExecutor, DesktopKlineFlashTransport, BdmKernelPlan>;

class EepromWorkflow final : public FlashWorkflow
{
  public:
    explicit EepromWorkflow(FlashWorkflowRequest request) : request_(std::move(request))
    {
    }

    FlashWorkflowStep next() override
    {
        if (request_.operation != FlashOperation::Read)
        {
            return FlashFailureStep{Error{ErrorKind::Unsupported, "EEPROM workflows support read operations only"}};
        }
        if (auto failure = outcome_.takeFailure(); failure.has_value())
        {
            return std::move(*failure);
        }
        if (outcome_.terminal())
        {
            return outcome_.completedStep();
        }
        if (!begun_)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (need_cycle_)
        {
            return FlashPromptStep{FlashPromptKind::CycleIgnition, {}};
        }
        if (inspect_)
        {
            return FlashPromptStep{FlashPromptKind::InspectRead, {}};
        }

        QtFileRepository repository;
        auto plan = build_eeprom_read_plan(request_.paths, request_.protocol, mode_, repository);
        if (!plan)
        {
            return FlashFailureStep{plan.error()};
        }
        if (plan->transport() == TransportKind::Kline)
        {
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     bind_flash_attempt(std::move(*plan),
                                                        std::make_unique<DensoSh705xEepromKlineExecutor>(),
                                                        std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                                     std::make_unique<QtClock>()};
        }
        return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                 bind_flash_attempt(std::move(*plan), std::make_unique<DensoSh705xEepromCanExecutor>(),
                                                    std::make_unique<DesktopCanFlashTransport>(request_.serial)),
                                 std::make_unique<QtClock>()};
    }

    void submit(FlashPromptResponse response) override
    {
        if (!begun_)
        {
            if (response == FlashPromptResponse::Accept)
            {
                begun_ = true;
            }
            else
            {
                outcome_.cancel();
            }
            return;
        }
        if (need_cycle_)
        {
            need_cycle_ = false;
            if (response != FlashPromptResponse::Accept)
            {
                outcome_.cancel();
            }
            return;
        }
        if (inspect_)
        {
            inspect_ = false;
            if (response == FlashPromptResponse::Save)
            {
                outcome_.succeed(std::move(pending_));
            }
            else if (mode_ == EepromReadMode::Mode4)
            {
                outcome_.discard();
            }
            else
            {
                advance();
                need_cycle_ = true;
                pending_.reset();
            }
        }
    }

    void submit(FlashAttemptResult result) override
    {
        if (result.success)
        {
            pending_ = std::move(result.read_bytes);
            inspect_ = true;
        }
        else if (result.error_kind == ErrorKind::Cancelled)
        {
            outcome_.cancel();
        }
        else if (mode_ != EepromReadMode::Mode4)
        {
            advance();
        }
        else
        {
            outcome_.fail(Error{result.error_kind, std::move(result.error_detail)});
        }
    }

  private:
    void advance()
    {
        using enum EepromReadMode;
        mode_ = mode_ == Mode2 ? Mode3 : Mode4;
    }

    FlashWorkflowRequest request_;
    EepromReadMode mode_ = EepromReadMode::Mode2;
    bool begun_ = false;
    bool need_cycle_ = false;
    bool inspect_ = false;
    std::optional<bytes::Bytes> pending_;
    FlashAttemptOutcome outcome_;
};

enum class RouteMatch
{
    Prefix,
    Exact,
};

struct Route
{
    enum class Kind
    {
        Colt,
        Eeprom,
        SubaruMitsuM32rKline,
        SubaruHitachiM32rKline,
        SubaruDensoMc68hc16y5_02,
        SubaruDensoSh7055_02,
        SubaruDensoSh705xDensoCan,
        SubaruTcuDensoSh705xCan,
        SubaruDensoSh7058Can,
        SubaruDensoSh7058CanDiesel,
        SubaruHitachiM32rCan,
        SubaruTcuHitachiM32rKline,
        SubaruUnisiaJecs,
        SubaruTcuHitachiM32rCan,
        SubaruHitachiSh72543rCan,
        SubaruHitachiSh7058,
        SubaruTcuCvtHitachiM32rCan,
        SubaruTcuCvtMitsuMh8111Can,
        SubaruTcuCvtMitsuMh8104Can,
        SubaruDenso1n83m_1_5mCan,
        SubaruDensoSh72531Can,
        SubaruDensoSh72543CanDiesel,
        SubaruDenso1n83m_4mCan,
        SubaruDensoSh705xKline,
        SubaruDensoMc68hc16y5_02Bdm,
        SubaruUnisiaJecsM32rKline,
        SubaruUnisiaJecsM32rBootMode,
        Unrouted,
    };

    std::string_view pattern;
    Kind kind;
    RouteMatch match = RouteMatch::Prefix;
};

using enum Route::Kind;

constexpr auto kRoutes = std::to_array<Route>({
    {"sub_ecu_hitachi_m32r_kline", SubaruHitachiM32rKline},
    {"sub_ecu_mitsu_m32r_kline", SubaruMitsuM32rKline},
    {"mitsu_ecu_m32r_can", Colt},
    {"sub_ecu_eeprom_denso_sh7055_kline", Eeprom},
    {"sub_ecu_eeprom_denso_sh7058_kline", Eeprom},
    {"sub_ecu_eeprom_denso_sh7055_densocan", Eeprom},
    {"sub_ecu_eeprom_denso_sh7058_densocan", Eeprom},
    {"sub_ecu_eeprom_denso_sh7058_can_diesel", Eeprom},
    {"sub_ecu_eeprom_denso_sh7058_can", Eeprom},
    {"sub_ecu_denso_sh7055_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058s_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058s_diesel_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
    {"sub_ecu_denso_sh7059_diesel_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
    {"sub_tcu_denso_sh7055_can", SubaruTcuDensoSh705xCan, RouteMatch::Exact},
    {"sub_tcu_denso_sh7058_can", SubaruTcuDensoSh705xCan, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_can", SubaruDensoSh7058Can, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_can_ecutek", SubaruDensoSh7058Can, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", SubaruDensoSh7058Can, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", SubaruDensoSh7058Can, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_can_cobb", SubaruDensoSh7058Can, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_can_diesel", SubaruDensoSh7058CanDiesel, RouteMatch::Exact},
    {"sub_ecu_denso_sh7059_can_diesel", SubaruDensoSh7058CanDiesel, RouteMatch::Exact},
    // Keep this longer prefix before the bare MC68 _02 row so no _02_bdm*
    // name reaches the K-Line family; the BDM plan rejects all but the exact
    // protocol.
    {"sub_ecu_denso_mc68hc16y5_02_bdm", SubaruDensoMc68hc16y5_02Bdm},
    {"sub_ecu_denso_mc68hc16y5_02", SubaruDensoMc68hc16y5_02},
    {"sub_ecu_denso_mc68hc16y5_04", SubaruDensoMc68hc16y5_02},
    {"sub_ecu_denso_sh7055_02", SubaruDensoSh7055_02},
    {"sub_ecu_denso_sh7055_04", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7055_04_ecutek", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7055_04_cobb", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_ecutek", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_denso_sh7058_cobb", SubaruDensoSh705xKline, RouteMatch::Exact},
    {"sub_ecu_hitachi_m32r_can", SubaruHitachiM32rCan},
    {"sub_tcu_hitachi_m32r_kline", SubaruTcuHitachiM32rKline, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_m3779x", SubaruUnisiaJecs, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_m3775x", SubaruUnisiaJecs, RouteMatch::Exact},
    // Exact only: the _bootmode names share these prefixes.
    {"sub_ecu_unisia_jecs_20", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_30", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_40", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_70", SubaruUnisiaJecsM32rKline, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_20_bootmode", SubaruUnisiaJecsM32rBootMode, RouteMatch::Exact},
    {"sub_ecu_unisia_jecs_30_bootmode", SubaruUnisiaJecsM32rBootMode, RouteMatch::Exact},
    {"sub_tcu_hitachi_m32r_can", SubaruTcuHitachiM32rCan, RouteMatch::Exact},
    {"sub_ecu_hitachi_sh72543r_can", SubaruHitachiSh72543rCan, RouteMatch::Exact},
    {"sub_ecu_hitachi_sh72543r_can_recovery", SubaruHitachiSh72543rCan, RouteMatch::Exact},
    {"sub_ecu_hitachi_sh7058_can", SubaruHitachiSh7058, RouteMatch::Exact},
    {"sub_tcu_cvt_hitachi_m32r_can", SubaruTcuCvtHitachiM32rCan},
    {"sub_tcu_cvt_mitsu_mh8111_can", SubaruTcuCvtMitsuMh8111Can},
    {"sub_tcu_cvt_mitsu_mh8104_can", SubaruTcuCvtMitsuMh8104Can},
    {"sub_ecu_denso_1n83m_1_5m_can", SubaruDenso1n83m_1_5mCan},
    {"sub_ecu_denso_sh72531_can", SubaruDensoSh72531Can},
    {"sub_ecu_denso_sh72543_can_diesel", SubaruDensoSh72543CanDiesel},
    // kRoutes is matched by starts_with; "sub_ecu_denso_1n83m_1_5m_can" and
    // "sub_ecu_denso_1n83m_4m_can" are not prefixes of one another, so the
    // order of these two entries relative to each other does not matter.
    {"sub_ecu_denso_1n83m_4m_can", SubaruDenso1n83m_4mCan},
});

} // namespace

std::optional<bytes::Bytes> portableImageForOperation(FlashOperation operation, bytes::ByteView rom)
{
    if (operation == FlashOperation::Read)
    {
        return std::nullopt;
    }
    return bytes::Bytes(rom.begin(), rom.end());
}

std::unique_ptr<FlashWorkflow> FlashWorkflowFactory::tryCreate(FlashWorkflowRequest request)
{
    const auto route = std::ranges::find_if(kRoutes,
                                            [&request](const Route& candidate)
                                            {
                                                return candidate.match == RouteMatch::Exact
                                                           ? request.protocol == candidate.pattern
                                                           : request.protocol.starts_with(candidate.pattern);
                                            });
    if (route == kRoutes.end())
    {
        return nullptr;
    }

    switch (route->kind)
    {
    case Colt:
        return std::make_unique<ColtWorkflow>(std::move(request));
    case Eeprom:
        return std::make_unique<EepromWorkflow>(std::move(request));
    case SubaruMitsuM32rKline:
        return std::make_unique<SubaruMitsuM32rKlineWorkflow>(std::move(request));
    case SubaruHitachiM32rKline:
        return std::make_unique<SubaruHitachiM32rKlineWorkflow>(std::move(request));
    case SubaruDensoMc68hc16y5_02:
        return std::make_unique<SubaruDensoMc68hc16y5_02Workflow>(std::move(request));
    case SubaruDensoSh7055_02:
        return std::make_unique<SubaruDensoSh7055_02Workflow>(std::move(request));
    case SubaruDensoSh705xDensoCan:
        return std::make_unique<SubaruDensoSh705xDensoCanWorkflow>(std::move(request));
    case SubaruTcuDensoSh705xCan:
        return std::make_unique<SubaruTcuDensoSh705xCanWorkflow>(std::move(request));
    case SubaruDensoSh7058Can:
        return std::make_unique<SubaruDensoSh7058CanWorkflow>(std::move(request));
    case SubaruDensoSh7058CanDiesel:
        return std::make_unique<SubaruDensoSh7058CanDieselWorkflow>(std::move(request));
    case SubaruHitachiM32rCan:
        return std::make_unique<SubaruHitachiM32rCanWorkflow>(std::move(request));
    case SubaruTcuHitachiM32rKline:
        return std::make_unique<SubaruTcuHitachiM32rKlineWorkflow>(std::move(request));
    case SubaruUnisiaJecs:
        return std::make_unique<SubaruUnisiaJecsWorkflow>(std::move(request));
    case SubaruHitachiSh72543rCan:
        return std::make_unique<SubaruHitachiSh72543rCanWorkflow>(std::move(request));
    case SubaruHitachiSh7058:
        if (request.operation == FlashOperation::Read)
        {
            return std::make_unique<SubaruHitachiSh7058KlineWorkflow>(std::move(request));
        }
        return std::make_unique<SubaruHitachiSh7058CanWorkflow>(std::move(request));
    case SubaruTcuHitachiM32rCan:
        return std::make_unique<SubaruTcuHitachiM32rCanWorkflow>(std::move(request));
    case SubaruTcuCvtHitachiM32rCan:
        return std::make_unique<SubaruTcuCvtHitachiM32rCanWorkflow>(std::move(request));
    case SubaruTcuCvtMitsuMh8111Can:
        return std::make_unique<SubaruTcuCvtMitsuMh8111CanWorkflow>(std::move(request));
    case SubaruTcuCvtMitsuMh8104Can:
        return std::make_unique<SubaruTcuCvtMitsuMh8104CanWorkflow>(std::move(request));
    case SubaruDenso1n83m_1_5mCan:
        return std::make_unique<SubaruDenso1n83m_1_5mCanWorkflow>(std::move(request));
    case SubaruDensoSh72531Can:
        return std::make_unique<SubaruDensoSh72531CanWorkflow>(std::move(request));
    case SubaruDensoSh72543CanDiesel:
        return std::make_unique<SubaruDensoSh72543CanDieselWorkflow>(std::move(request));
    case SubaruDenso1n83m_4mCan:
        return std::make_unique<SubaruDenso1n83m_4mCanWorkflow>(std::move(request));
    case SubaruDensoSh705xKline:
        return std::make_unique<SubaruDensoSh705xKlineWorkflow>(std::move(request));
    case SubaruDensoMc68hc16y5_02Bdm:
        return std::make_unique<SubaruDensoMc68hc16y5_02BdmWorkflow>(std::move(request));
    case SubaruUnisiaJecsM32rKline:
        return std::make_unique<SubaruUnisiaJecsM32rKlineWorkflow>(std::move(request));
    case SubaruUnisiaJecsM32rBootMode:
        return std::make_unique<SubaruUnisiaJecsM32rBootModeWorkflow>(std::move(request));
    case Unrouted:
        return nullptr;
    }
    assert(false);
    return nullptr;
}

} // namespace fastecu::flash
