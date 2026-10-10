#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <format>
#include <string_view>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_image.h"
#include "src/backend/config/catalog.h"
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

FlashCompletedStep Completed(FlashWorkflowOutcome outcome, std::optional<bytes::Bytes> bytes = std::nullopt,
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
    void Succeed(std::optional<bytes::Bytes> bytes = std::nullopt, std::optional<std::string> rom_id = std::nullopt)
    {
        terminal_ = true;
        outcome_ = FlashWorkflowOutcome::kSucceeded;
        bytes_ = std::move(bytes);
        rom_id_ = std::move(rom_id);
    }
    void Cancel()
    {
        terminal_ = true;
        outcome_ = FlashWorkflowOutcome::kCancelled;
    }
    void Discard()
    {
        terminal_ = true;
        outcome_ = FlashWorkflowOutcome::kDiscarded;
    }
    void Fail(Error error)
    {
        terminal_ = true;
        outcome_ = FlashWorkflowOutcome::kFailed;
        failure_ = std::move(error);
    }

    // The success/cancelled/failed mapping shared by every workflow whose
    // attempt result finalizes the outcome directly.
    void Record(FlashAttemptResult result)
    {
        if (result.success)
        {
            Succeed(std::move(result.read_bytes), std::move(result.rom_id));
        }
        else if (result.error_kind == ErrorKind::kCancelled)
        {
            Cancel();
        }
        else
        {
            Fail(Error{result.error_kind, std::move(result.error_detail)});
        }
    }

    bool Terminal() const
    {
        return terminal_;
    }
    // The recorded failure, moved out; empty if the workflow has not failed.
    std::optional<FlashFailureStep> TakeFailure()
    {
        if (!failure_.has_value())
        {
            return std::nullopt;
        }
        return FlashFailureStep{std::move(*failure_)};
    }
    FlashCompletedStep CompletedStep()
    {
        return Completed(outcome_, std::move(bytes_), std::move(rom_id_));
    }

  private:
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::kFailed;
    std::optional<bytes::Bytes> bytes_;
    std::optional<std::string> rom_id_;
    std::optional<Error> failure_;
};

Result<KernelImage> ResolveKernel(const FlashWorkflowRequest& request, IFileRepository& repository)
{
    if (!request.protocol.kernel_load_address.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("protocol '{}' declares no kernel load address", request.protocol.name));
    }
    Result<std::vector<std::uint8_t>> kernel_bytes =
        repository.Read(request.paths.kernel_files_directory + std::string(request.protocol.kernel));
    if (!kernel_bytes.has_value())
    {
        return std::unexpected(kernel_bytes.error());
    }
    return KernelImage{.id = std::format("{}-kernel", request.protocol.name),
                       .load_address = *request.protocol.kernel_load_address,
                       .bytes = std::move(*kernel_bytes)};
}

// The kernel file alone. The Unisia Jecs M32R _bootmode entries declare no
// kernel load address: the M32R boot ROM places the kernel itself.
Result<bytes::Bytes> ResolveKernelBytes(const FlashWorkflowRequest& request, IFileRepository& repository)
{
    Result<std::vector<std::uint8_t>> kernel_bytes =
        repository.Read(request.paths.kernel_files_directory + std::string(request.protocol.kernel));
    if (!kernel_bytes.has_value())
    {
        const Error& error = kernel_bytes.error();
        return Fail(error.kind, std::format("kernel file '{}': {}", request.protocol.kernel, error.detail));
    }
    return bytes::Bytes(kernel_bytes->begin(), kernel_bytes->end());
}

// The adapter check moved here from legacy write_mem() :434: an
// adapter that supplies programming voltage needs no operator prompt. With no
// serial at all the workflow cannot know, so it prompts.
class SubaruUnisiaJecsM32rKlineWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruUnisiaJecsM32rKlineWorkflow(FlashWorkflowRequest request)
        : request_(std::move(request)),
          plan_(BuildSubaruUnisiaJecsM32rKlinePlan(request_.operation, request_.protocol.name, request_.protocol.mcu,
                                                   std::move(request_.image),
                                                   AdapterSuppliesProgrammingVoltage(request_.serial)))
    {
        if (plan_.has_value())
        {
            needs_vpp_ = std::ranges::any_of(plan_->Confirmations(), [](const ConfirmationSpec& spec)
                                             { return spec.id == ConfirmationSpec::Id::kApplyProgrammingVoltage; });
        }
    }

    FlashWorkflowStep Next() override
    {
        if (!plan_.has_value())
        {
            return FlashFailureStep{plan_.error()};
        }
        // The reminder precedes whatever the attempt produced, failure included.
        if (stage_ == Stage::kRemoveVpp)
        {
            return FlashPromptStep{FlashPromptKind::kRemoveProgrammingVoltage,
                                   {{"outcome", attempt_outcome_}, {"external_vpp", needs_vpp_ ? "yes" : "no"}}};
        }
        if (auto failure = outcome_.TakeFailure(); failure.has_value())
        {
            return std::move(*failure);
        }
        if (outcome_.Terminal())
        {
            return outcome_.CompletedStep();
        }
        if (stage_ == Stage::kBegin)
        {
            return FlashPromptStep{FlashPromptKind::kBegin, {}};
        }
        if (stage_ == Stage::kApplyVpp)
        {
            return FlashPromptStep{FlashPromptKind::kApplyProgrammingVoltage, {}};
        }
        if (stage_ == Stage::kAttempt)
        {
            stage_ = Stage::kDone;
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     BindFlashAttempt(std::move(*plan_),
                                                      std::make_unique<SubaruUnisiaJecsM32rKlineExecutor>(),
                                                      std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                                     std::make_unique<QtClock>()};
        }
        return outcome_.CompletedStep();
    }

    void Submit(FlashPromptResponse response) override
    {
        switch (stage_)
        {
        case Stage::kBegin:
        case Stage::kApplyVpp:
            if (response != FlashPromptResponse::kAccept)
            {
                outcome_.Cancel();
                return;
            }
            stage_ = stage_ == Stage::kBegin && needs_vpp_ ? Stage::kApplyVpp : Stage::kAttempt;
            return;
        case Stage::kRemoveVpp:
            stage_ = Stage::kDone; // OK-only notice
            return;
        case Stage::kAttempt:
        case Stage::kDone:
            return;
        }
    }

    void Submit(FlashAttemptResult result) override
    {
        // Legacy warned after every failed write, whatever the adapter, not to
        // power off the ECU; the remove-VPP sentence is due only when the
        // operator applied external VPP, success included.
        if (is_write_ && (needs_vpp_ || !result.success))
        {
            attempt_outcome_ = result.success                               ? "succeeded"
                               : result.error_kind == ErrorKind::kCancelled ? "cancelled"
                                                                            : "failed";
            stage_ = Stage::kRemoveVpp;
        }
        outcome_.Record(std::move(result));
    }

  private:
    enum class Stage
    {
        kBegin,
        kApplyVpp,
        kAttempt,
        kRemoveVpp,
        kDone,
    };

    FlashWorkflowRequest request_;
    Result<FlashPlan> plan_;
    bool is_write_ = request_.operation == FlashOperation::kWrite;
    bool needs_vpp_ = false;
    Stage stage_ = Stage::kBegin;
    std::string attempt_outcome_;
    FlashAttemptOutcome outcome_;
};

// Read is the 6c-3 K-Line read. Write is two attempts with an
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

    FlashWorkflowStep Next() override
    {
        if (!built_.has_value())
        {
            built_ = BuildPlans();
        }
        if (!built_->has_value())
        {
            return FlashFailureStep{built_->error()};
        }
        // The notice precedes whatever the attempt produced, failure included.
        if (stage_ == Stage::kNotice)
        {
            return FlashPromptStep{FlashPromptKind::kRemoveProgrammingVoltage,
                                   {{"outcome", notice_outcome_}, {"external_vpp", "yes"}, {"power_off_advice", "no"}}};
        }
        if (auto failure = outcome_.TakeFailure(); failure.has_value())
        {
            return std::move(*failure);
        }
        if (outcome_.Terminal())
        {
            return outcome_.CompletedStep();
        }
        switch (stage_)
        {
        case Stage::kBegin:
            return FlashPromptStep{FlashPromptKind::kBegin, {}};
        case Stage::kApplyVoltages:
            return FlashPromptStep{FlashPromptKind::kApplyBootModeVoltages, {}};
        case Stage::kFirstAttempt:
            stage_ = Stage::kAwaitFirst;
            return FirstAttempt();
        case Stage::kRemoveMod1:
            return FlashPromptStep{FlashPromptKind::kRemoveMod1, {}};
        case Stage::kProgramAttempt:
        {
            std::optional<FlashPlan>& program = (*built_)->program;
            if (!program.has_value())
            {
                return FlashFailureStep{Error{ErrorKind::kInternal, "the Unisia JECS program plan was not built"}};
            }
            stage_ = Stage::kAwaitProgram;
            return Attempt(std::move(*program), std::make_unique<SubaruUnisiaJecsM32rBootModeProgramExecutor>());
        }
        case Stage::kAwaitFirst:
        case Stage::kAwaitProgram:
        case Stage::kNotice:
        case Stage::kDone:
            break;
        }
        return outcome_.CompletedStep();
    }

    void Submit(FlashPromptResponse response) override
    {
        switch (stage_)
        {
        case Stage::kBegin:
        case Stage::kApplyVoltages:
            if (response != FlashPromptResponse::kAccept)
            {
                outcome_.Cancel();
                return;
            }
            stage_ = stage_ == Stage::kBegin && IsWrite() ? Stage::kApplyVoltages : Stage::kFirstAttempt;
            return;
        case Stage::kRemoveMod1:
            if (response != FlashPromptResponse::kAccept)
            {
                // The kernel runs and nothing is erased; still ask for VPP removal.
                notice_outcome_ = "cancelled";
                stage_ = Stage::kNotice;
                outcome_.Cancel();
                return;
            }
            stage_ = Stage::kProgramAttempt;
            return;
        case Stage::kNotice:
            stage_ = Stage::kDone; // OK-only notice
            return;
        case Stage::kFirstAttempt:
        case Stage::kAwaitFirst:
        case Stage::kProgramAttempt:
        case Stage::kAwaitProgram:
        case Stage::kDone:
            return;
        }
    }

    void Submit(FlashAttemptResult result) override
    {
        if (!IsWrite())
        {
            outcome_.Record(std::move(result));
            return;
        }
        // A successful kernel upload is not an outcome yet: RemoveMod1 follows.
        if (stage_ == Stage::kAwaitFirst && result.success)
        {
            stage_ = Stage::kRemoveMod1;
            return;
        }
        notice_outcome_ = result.success                               ? "succeeded"
                          : result.error_kind == ErrorKind::kCancelled ? "cancelled"
                                                                       : "failed";
        stage_ = Stage::kNotice;
        outcome_.Record(std::move(result));
    }

  private:
    enum class Stage
    {
        kBegin,
        kApplyVoltages,
        kFirstAttempt,
        kAwaitFirst,
        kRemoveMod1,
        kProgramAttempt,
        kAwaitProgram,
        kNotice,
        kDone,
    };

    struct Plans
    {
        std::optional<FlashPlan> first;   // Read plan, or the kernel upload
        std::optional<FlashPlan> program; // Write only
    };

    bool IsWrite() const
    {
        return request_.operation != FlashOperation::kRead;
    }

    Result<Plans> BuildPlans()
    {
        if (!IsWrite())
        {
            // adapter_supplies_programming_voltage is irrelevant to Read.
            auto read = BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kRead, request_.protocol.name,
                                                           request_.protocol.mcu, std::nullopt, true);
            if (!read.has_value())
            {
                return std::unexpected(read.error());
            }
            return Plans{std::move(*read), std::nullopt};
        }
        // Program first: it needs no I/O and rejects TestWrite and a wrong
        // image before the kernel file is read.
        auto program = BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(request_.operation, request_.protocol.name,
                                                                    request_.protocol.mcu, std::move(request_.image));
        if (!program.has_value())
        {
            return std::unexpected(program.error());
        }
        QtFileRepository repository;
        Result<bytes::Bytes> kernel_bytes = ResolveKernelBytes(request_, repository);
        if (!kernel_bytes.has_value())
        {
            return std::unexpected(kernel_bytes.error());
        }
        auto kernel = BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(request_.operation, request_.protocol.name,
                                                                  request_.protocol.mcu, std::move(*kernel_bytes));
        if (!kernel.has_value())
        {
            return std::unexpected(kernel.error());
        }
        return Plans{std::move(*kernel), std::move(*program)};
    }

    FlashWorkflowStep FirstAttempt()
    {
        if (!built_.has_value() || !built_->has_value())
        {
            return FlashFailureStep{Error{ErrorKind::kInternal, "the Unisia JECS plans were not built"}};
        }
        std::optional<FlashPlan>& first = (*built_)->first;
        if (!first.has_value())
        {
            return FlashFailureStep{Error{ErrorKind::kInternal, "the Unisia JECS first-stage plan was not built"}};
        }
        FlashPlan plan = std::move(*first);
        if (!IsWrite())
        {
            return Attempt(std::move(plan), std::make_unique<SubaruUnisiaJecsM32rKlineExecutor>());
        }
        return Attempt(std::move(plan), std::make_unique<SubaruUnisiaJecsM32rBootModeKernelExecutor>());
    }

    template <typename Executor> FlashWorkflowStep Attempt(FlashPlan plan, std::unique_ptr<Executor> executor)
    {
        return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                 BindFlashAttempt(std::move(plan), std::move(executor),
                                                  std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                                 std::make_unique<QtClock>()};
    }

    FlashWorkflowRequest request_;
    std::optional<Result<Plans>> built_;
    Stage stage_ = Stage::kBegin;
    std::string notice_outcome_;
    FlashAttemptOutcome outcome_;
};

// The prompt that collects each confirmation a single-attempt plan can carry,
// arguments unchanged. The other ids belong to workflows with their own
// staging; meeting one here is a routing defect, reported before any prompt
// or hardware access.
Result<FlashPromptStep> ConfirmationPrompt(const ConfirmationSpec& confirmation)
{
    using enum ConfirmationSpec::Id;
    switch (confirmation.id)
    {
    case kCycleIgnition:
        return FlashPromptStep{FlashPromptKind::kCycleIgnition, confirmation.arguments};
    case kEraseTrigger:
        return FlashPromptStep{FlashPromptKind::kColtEraseTrigger, confirmation.arguments};
    case kTopRegionBootstrap:
        return FlashPromptStep{FlashPromptKind::kColtTopRegionBootstrap, confirmation.arguments};
    case kStartKlineRead:
        return FlashPromptStep{FlashPromptKind::kConfirmSh7058Read, confirmation.arguments};
    case kKernelBootstrap:
        return FlashPromptStep{FlashPromptKind::kConfirmBdmKernelBootstrap, confirmation.arguments};
    case kBeginEepromRead:
    case kInspectEepromBytes:
    case kApplyProgrammingVoltage:
    case kApplyBootModeVoltages:
        break;
    }
    return Fail(ErrorKind::kInternal,
                std::format("confirmation {} has no single-attempt prompt", static_cast<int>(confirmation.id)));
}

// Begin, then one prompt per plan confirmation, in plan order.
Result<std::vector<FlashPromptStep>> PromptSequence(const FlashPlan& plan)
{
    std::vector<FlashPromptStep> prompts{FlashPromptStep{FlashPromptKind::kBegin, {}}};
    for (const ConfirmationSpec& confirmation : plan.Confirmations())
    {
        Result<FlashPromptStep> prompt = ConfirmationPrompt(confirmation);
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
        : plan_(Build(request.operation, request.protocol.name, request.protocol.mcu, std::move(request.image)))
    {
    }

    Result<FlashPlan>& Plan(const FlashWorkflowRequest&)
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

    Result<FlashPlan>& Plan(FlashWorkflowRequest& request)
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

// A kernel-backed family: the protocol's kernel file is read, then the family
// builder runs.
template <KernelBackedPlanBuilder Build> Result<FlashPlan> PrepareKernelBacked(FlashWorkflowRequest& request)
{
    QtFileRepository repository;
    Result<KernelImage> kernel = ResolveKernel(request, repository);
    if (!kernel.has_value())
    {
        return std::unexpected(kernel.error());
    }
    return Build(request.operation, request.protocol.name, request.protocol.mcu, std::move(request.image),
                 std::move(*kernel));
}

// MC68HC16Y5_02: prepareKernelBacked with two family steps before the kernel
// is read: the ROM file is placed by its protocol's memory map for its size
// (ADR 0020), and the family builder checks the protocol, MCU and placement.
Result<FlashPlan> PrepareMc68(FlashWorkflowRequest& request)
{
    std::optional<memory::MemoryImage> image;
    if (request.image.has_value())
    {
        auto placed = config::PlaceRomFile(&request.protocol, std::move(*request.image));
        if (!placed.has_value())
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("ROM file size error: {}", placed.error().detail));
        }
        image = std::move(*placed);
    }
    // Run the family builder first so a protocol or MCU the family rejects
    // fails before the kernel file is read.
    Result<FlashPlan> preflight = BuildSubaruDensoMc68hc16y502Plan(
        request.operation, request.protocol.name, request.protocol.mcu, image,
        KernelImage{.id = std::format("{}-kernel", request.protocol.name), .load_address = 0x20000, .bytes = {0}});
    if (!preflight.has_value())
    {
        return std::unexpected(preflight.error());
    }
    QtFileRepository repository;
    Result<KernelImage> kernel = ResolveKernel(request, repository);
    if (!kernel.has_value())
    {
        return std::unexpected(kernel.error());
    }
    return BuildSubaruDensoMc68hc16y502Plan(request.operation, request.protocol.name, request.protocol.mcu,
                                            std::move(image), std::move(*kernel));
}

// MC68HC16Y5 BDM: only Write reads the kernel file -- its "write" uploads and
// starts the protocol's kernel. The operator's ROM (request.image) is never
// forwarded: BDM never writes the ROM.
Result<FlashPlan> PrepareBdm(FlashWorkflowRequest& request)
{
    if (request.operation != FlashOperation::kWrite)
    {
        return BuildSubaruDensoMc68hc16y502BdmPlan(request.operation, request.protocol.name, request.protocol.mcu,
                                                   std::nullopt, std::nullopt);
    }
    QtFileRepository repository;
    Result<KernelImage> kernel = ResolveKernel(request, repository);
    if (!kernel.has_value())
    {
        return std::unexpected(kernel.error());
    }
    return BuildSubaruDensoMc68hc16y502BdmPlan(request.operation, request.protocol.name, request.protocol.mcu,
                                               std::nullopt, std::move(*kernel));
}

template <KernelBackedPlanBuilder Build> using CachedKernelPlan = LazyPlan<&PrepareKernelBacked<Build>>;
using Mc68KernelPlan = LazyPlan<&PrepareMc68>;
using BdmKernelPlan = LazyPlan<&PrepareBdm>;

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

    FlashWorkflowStep Next() override
    {
        Result<FlashPlan>& plan = preparation_.Plan(request_);
        if (!plan.has_value())
        {
            return FlashFailureStep{plan.error()};
        }
        if (!prompts_.has_value())
        {
            prompts_ = PromptSequence(*plan);
        }
        if (!prompts_->has_value())
        {
            return FlashFailureStep{prompts_->error()};
        }
        if (auto failure = outcome_.TakeFailure(); failure.has_value())
        {
            return std::move(*failure);
        }
        if (outcome_.Terminal())
        {
            return outcome_.CompletedStep();
        }
        if (const std::vector<FlashPromptStep>& prompts = **prompts_; accepted_ < prompts.size())
        {
            return prompts[accepted_];
        }
        if (!attempted_)
        {
            attempted_ = true;
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     BindFlashAttempt(std::move(*plan), std::make_unique<Executor>(),
                                                      std::make_unique<Transport>(request_.serial)),
                                     std::make_unique<QtClock>()};
        }
        return outcome_.CompletedStep();
    }

    void Submit(FlashPromptResponse response) override
    {
        if (response != FlashPromptResponse::kAccept)
        {
            outcome_.Cancel();
            return;
        }
        ++accepted_;
    }

    void Submit(FlashAttemptResult result) override
    {
        outcome_.Record(std::move(result));
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
using ColtWorkflow = KernelFreeCanWorkflow<MitsuColtM32rCanExecutor, &BuildMitsuColtM32rCanPlan>;
using SubaruHitachiM32rCanWorkflow =
    KernelFreeCanWorkflow<SubaruHitachiM32rCanExecutor, &BuildSubaruHitachiM32rCanPlan>;
using SubaruTcuCvtHitachiM32rCanWorkflow =
    KernelFreeCanWorkflow<SubaruTcuCvtHitachiM32rCanExecutor, &BuildSubaruTcuCvtHitachiM32rCanPlan>;
using SubaruTcuCvtMitsuMh8111CanWorkflow =
    KernelFreeCanWorkflow<SubaruTcuCvtMitsuMh8111CanExecutor, &BuildSubaruTcuCvtMitsuMh8111CanPlan>;
using SubaruTcuCvtMitsuMh8104CanWorkflow =
    KernelFreeCanWorkflow<SubaruTcuCvtMitsuMh8104CanExecutor, &BuildSubaruTcuCvtMitsuMh8104CanPlan>;
using SubaruDenso1n83m_1_5mCanWorkflow =
    KernelFreeCanWorkflow<SubaruDenso1n83m_1_5mCanExecutor, &BuildSubaruDenso1n83m15mCanPlan>;
using SubaruDensoSh72531CanWorkflow =
    KernelFreeCanWorkflow<SubaruDensoSh72531CanExecutor, &BuildSubaruDensoSh72531CanPlan>;
using SubaruDensoSh72543CanDieselWorkflow =
    KernelFreeCanWorkflow<SubaruDensoSh72543CanDieselExecutor, &BuildSubaruDensoSh72543CanDieselPlan>;
using SubaruDenso1n83m_4mCanWorkflow =
    KernelFreeCanWorkflow<SubaruDenso1n83m_4mCanExecutor, &BuildSubaruDenso1n83m4mCanPlan>;
// Supports Read and Write. TestWrite is rejected by the plan builder: the
// legacy reflash_block ignored its test_write_arg and erased and wrote for
// real, so there is no dry run to route to.
using SubaruTcuHitachiM32rCanWorkflow =
    KernelFreeCanWorkflow<SubaruTcuHitachiM32rCanExecutor, &BuildSubaruTcuHitachiM32rCanPlan>;
using SubaruHitachiSh72543rCanWorkflow =
    KernelFreeCanWorkflow<SubaruHitachiSh72543rCanExecutor, &BuildSubaruHitachiSh72543rCanPlan>;

using SubaruMitsuM32rKlineWorkflow =
    KernelFreeKlineWorkflow<SubaruMitsuM32rKlineExecutor, &BuildSubaruMitsuM32rKlinePlan>;
using SubaruHitachiM32rKlineWorkflow =
    KernelFreeKlineWorkflow<SubaruHitachiM32rKlineExecutor, &BuildSubaruHitachiM32rKlinePlan>;
using SubaruTcuHitachiM32rKlineWorkflow =
    KernelFreeKlineWorkflow<SubaruTcuHitachiM32rKlineExecutor, &BuildSubaruTcuHitachiM32rKlinePlan>;
using SubaruUnisiaJecsWorkflow = KernelFreeKlineWorkflow<SubaruUnisiaJecsExecutor, &BuildSubaruUnisiaJecsPlan>;

// DensoCAN plans carry exactly one CycleIgnition confirmation; the other
// kernel-backed plans carry none.
using SubaruDensoSh705xDensoCanWorkflow =
    KernelBackedWorkflow<SubaruDensoSh705xDensoCanExecutor, DesktopMixedCanFlashTransport,
                         &BuildSubaruDensoSh705xDensocanPlan>;
using SubaruTcuDensoSh705xCanWorkflow =
    KernelBackedWorkflow<SubaruTcuDensoSh705xCanExecutor, DesktopCanFlashTransport, &BuildSubaruTcuDensoSh705xCanPlan>;
using SubaruDensoSh7058CanWorkflow =
    KernelBackedWorkflow<SubaruDensoSh7058CanExecutor, DesktopCanFlashTransport, &BuildSubaruDensoSh7058CanPlan>;
using SubaruDensoSh7058CanDieselWorkflow =
    KernelBackedWorkflow<SubaruDensoSh7058CanDieselExecutor, DesktopCanFlashTransport,
                         &BuildSubaruDensoSh7058CanDieselPlan>;
// The sequence is the kernel resolved on the first step, the shared
// Begin prompt -- the legacy dialog's only prompt, "Turn ignition ON" -- then
// the attempt.
using SubaruDensoSh705xKlineWorkflow =
    KernelBackedWorkflow<SubaruDensoSh705xKlineExecutor, DesktopKlineFlashTransport, &BuildSubaruDensoSh705xKlinePlan>;
// SH7055_02 plans carry exactly one CycleIgnition confirmation.
using SubaruDensoSh7055_02Workflow =
    KernelBackedWorkflow<SubaruDensoSh7055_02Executor, DesktopKlineFlashTransport, &BuildSubaruDensoSh705502Plan>;
using SubaruDensoMc68hc16y5_02Workflow =
    SingleAttemptFlashWorkflow<SubaruDensoMc68hc16y5_02Executor, DesktopKlineFlashTransport, Mc68KernelPlan>;
// Hitachi SH7058 Read runs over K-Line and Write over CAN: the plan builder
// chooses the transport by operation and the factory the matching executor.
// Read plans carry StartKlineRead; write plans carry nothing.
using SubaruHitachiSh7058KlineWorkflow =
    KernelFreeKlineWorkflow<SubaruHitachiSh7058KlineExecutor, &BuildSubaruHitachiSh7058Plan>;
using SubaruHitachiSh7058CanWorkflow =
    KernelFreeCanWorkflow<SubaruHitachiSh7058CanExecutor, &BuildSubaruHitachiSh7058Plan>;
// Write plans carry KernelBootstrap; read plans carry nothing.
using SubaruDensoMc68hc16y5_02BdmWorkflow =
    SingleAttemptFlashWorkflow<SubaruDensoMc68hc16y5_02BdmExecutor, DesktopKlineFlashTransport, BdmKernelPlan>;

class EepromWorkflow final : public FlashWorkflow
{
  public:
    explicit EepromWorkflow(FlashWorkflowRequest request) : request_(std::move(request))
    {
    }

    FlashWorkflowStep Next() override
    {
        if (request_.operation != FlashOperation::kRead)
        {
            return FlashFailureStep{Error{ErrorKind::kUnsupported, "EEPROM workflows support read operations only"}};
        }
        if (auto failure = outcome_.TakeFailure(); failure.has_value())
        {
            return std::move(*failure);
        }
        if (outcome_.Terminal())
        {
            return outcome_.CompletedStep();
        }
        if (!begun_)
        {
            return FlashPromptStep{FlashPromptKind::kBegin, {}};
        }
        if (need_cycle_)
        {
            return FlashPromptStep{FlashPromptKind::kCycleIgnition, {}};
        }
        if (inspect_)
        {
            return FlashPromptStep{FlashPromptKind::kInspectRead, {}};
        }

        QtFileRepository repository;
        auto plan = BuildEepromReadPlan(request_.paths, request_.protocol, mode_, repository);
        if (!plan)
        {
            return FlashFailureStep{plan.error()};
        }
        if (plan->Transport() == TransportKind::kKline)
        {
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     BindFlashAttempt(std::move(*plan),
                                                      std::make_unique<DensoSh705xEepromKlineExecutor>(),
                                                      std::make_unique<DesktopKlineFlashTransport>(request_.serial)),
                                     std::make_unique<QtClock>()};
        }
        return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                 BindFlashAttempt(std::move(*plan), std::make_unique<DensoSh705xEepromCanExecutor>(),
                                                  std::make_unique<DesktopCanFlashTransport>(request_.serial)),
                                 std::make_unique<QtClock>()};
    }

    void Submit(FlashPromptResponse response) override
    {
        if (!begun_)
        {
            if (response == FlashPromptResponse::kAccept)
            {
                begun_ = true;
            }
            else
            {
                outcome_.Cancel();
            }
            return;
        }
        if (need_cycle_)
        {
            need_cycle_ = false;
            if (response != FlashPromptResponse::kAccept)
            {
                outcome_.Cancel();
            }
            return;
        }
        if (inspect_)
        {
            inspect_ = false;
            if (response == FlashPromptResponse::kSave)
            {
                outcome_.Succeed(std::move(pending_));
            }
            else if (mode_ == EepromReadMode::kMode4)
            {
                outcome_.Discard();
            }
            else
            {
                Advance();
                need_cycle_ = true;
                pending_.reset();
            }
        }
    }

    void Submit(FlashAttemptResult result) override
    {
        if (result.success)
        {
            pending_ = std::move(result.read_bytes);
            inspect_ = true;
        }
        else if (result.error_kind == ErrorKind::kCancelled)
        {
            outcome_.Cancel();
        }
        else if (mode_ != EepromReadMode::kMode4)
        {
            Advance();
        }
        else
        {
            outcome_.Fail(Error{result.error_kind, std::move(result.error_detail)});
        }
    }

  private:
    void Advance()
    {
        using enum EepromReadMode;
        mode_ = mode_ == kMode2 ? kMode3 : kMode4;
    }

    FlashWorkflowRequest request_;
    EepromReadMode mode_ = EepromReadMode::kMode2;
    bool begun_ = false;
    bool need_cycle_ = false;
    bool inspect_ = false;
    std::optional<bytes::Bytes> pending_;
    FlashAttemptOutcome outcome_;
};

enum class RouteMatch
{
    kPrefix,
    kExact,
};

struct Route
{
    enum class Kind
    {
        kColt,
        kEeprom,
        kSubaruMitsuM32rKline,
        kSubaruHitachiM32rKline,
        kSubaruDensoMc68hc16y502,
        kSubaruDensoSh705502,
        kSubaruDensoSh705xDensoCan,
        kSubaruTcuDensoSh705xCan,
        kSubaruDensoSh7058Can,
        kSubaruDensoSh7058CanDiesel,
        kSubaruHitachiM32rCan,
        kSubaruTcuHitachiM32rKline,
        kSubaruUnisiaJecs,
        kSubaruTcuHitachiM32rCan,
        kSubaruHitachiSh72543rCan,
        kSubaruHitachiSh7058,
        kSubaruTcuCvtHitachiM32rCan,
        kSubaruTcuCvtMitsuMh8111Can,
        kSubaruTcuCvtMitsuMh8104Can,
        kSubaruDenso1n83m15mCan,
        kSubaruDensoSh72531Can,
        kSubaruDensoSh72543CanDiesel,
        kSubaruDenso1n83m4mCan,
        kSubaruDensoSh705xKline,
        kSubaruDensoMc68hc16y502Bdm,
        kSubaruUnisiaJecsM32rKline,
        kSubaruUnisiaJecsM32rBootMode,
        kUnrouted,
    };

    std::string_view pattern;
    Kind kind;
    RouteMatch match = RouteMatch::kPrefix;
};

using enum Route::Kind;

constexpr auto kRoutes = std::to_array<Route>({
    {"sub_ecu_hitachi_m32r_kline", kSubaruHitachiM32rKline},
    {"sub_ecu_mitsu_m32r_kline", kSubaruMitsuM32rKline},
    {"mitsu_ecu_m32r_can", kColt},
    {"sub_ecu_eeprom_denso_sh7055_kline", kEeprom},
    {"sub_ecu_eeprom_denso_sh7058_kline", kEeprom},
    {"sub_ecu_eeprom_denso_sh7055_densocan", kEeprom},
    {"sub_ecu_eeprom_denso_sh7058_densocan", kEeprom},
    {"sub_ecu_eeprom_denso_sh7058_can_diesel", kEeprom},
    {"sub_ecu_eeprom_denso_sh7058_can", kEeprom},
    {"sub_ecu_denso_sh7055_densocan", kSubaruDensoSh705xDensoCan, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058_densocan", kSubaruDensoSh705xDensoCan, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058s_densocan", kSubaruDensoSh705xDensoCan, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058s_diesel_densocan", kSubaruDensoSh705xDensoCan, RouteMatch::kExact},
    {"sub_ecu_denso_sh7059_diesel_densocan", kSubaruDensoSh705xDensoCan, RouteMatch::kExact},
    {"sub_tcu_denso_sh7055_can", kSubaruTcuDensoSh705xCan, RouteMatch::kExact},
    {"sub_tcu_denso_sh7058_can", kSubaruTcuDensoSh705xCan, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058_can", kSubaruDensoSh7058Can, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058_can_ecutek", kSubaruDensoSh7058Can, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", kSubaruDensoSh7058Can, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", kSubaruDensoSh7058Can, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058_can_cobb", kSubaruDensoSh7058Can, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058_can_diesel", kSubaruDensoSh7058CanDiesel, RouteMatch::kExact},
    {"sub_ecu_denso_sh7059_can_diesel", kSubaruDensoSh7058CanDiesel, RouteMatch::kExact},
    // Keep this longer prefix before the bare MC68 _02 row so no _02_bdm*
    // name reaches the K-Line family; the BDM plan rejects all but the exact
    // protocol.
    {"sub_ecu_denso_mc68hc16y5_02_bdm", kSubaruDensoMc68hc16y502Bdm},
    {"sub_ecu_denso_mc68hc16y5_02", kSubaruDensoMc68hc16y502},
    {"sub_ecu_denso_sh7055_02", kSubaruDensoSh705502},
    {"sub_ecu_denso_sh7055_04", kSubaruDensoSh705xKline, RouteMatch::kExact},
    {"sub_ecu_denso_sh7055_04_ecutek", kSubaruDensoSh705xKline, RouteMatch::kExact},
    {"sub_ecu_denso_sh7055_04_cobb", kSubaruDensoSh705xKline, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058", kSubaruDensoSh705xKline, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058_ecutek", kSubaruDensoSh705xKline, RouteMatch::kExact},
    {"sub_ecu_denso_sh7058_cobb", kSubaruDensoSh705xKline, RouteMatch::kExact},
    {"sub_ecu_hitachi_m32r_can", kSubaruHitachiM32rCan},
    {"sub_tcu_hitachi_m32r_kline", kSubaruTcuHitachiM32rKline, RouteMatch::kExact},
    {"sub_ecu_unisia_jecs_m3779x", kSubaruUnisiaJecs, RouteMatch::kExact},
    {"sub_ecu_unisia_jecs_m3775x", kSubaruUnisiaJecs, RouteMatch::kExact},
    // Exact only: the _bootmode names share these prefixes.
    {"sub_ecu_unisia_jecs_20", kSubaruUnisiaJecsM32rKline, RouteMatch::kExact},
    {"sub_ecu_unisia_jecs_30", kSubaruUnisiaJecsM32rKline, RouteMatch::kExact},
    {"sub_ecu_unisia_jecs_40", kSubaruUnisiaJecsM32rKline, RouteMatch::kExact},
    {"sub_ecu_unisia_jecs_70", kSubaruUnisiaJecsM32rKline, RouteMatch::kExact},
    {"sub_ecu_unisia_jecs_20_bootmode", kSubaruUnisiaJecsM32rBootMode, RouteMatch::kExact},
    {"sub_ecu_unisia_jecs_30_bootmode", kSubaruUnisiaJecsM32rBootMode, RouteMatch::kExact},
    {"sub_tcu_hitachi_m32r_can", kSubaruTcuHitachiM32rCan, RouteMatch::kExact},
    {"sub_ecu_hitachi_sh72543r_can", kSubaruHitachiSh72543rCan, RouteMatch::kExact},
    {"sub_ecu_hitachi_sh72543r_can_recovery", kSubaruHitachiSh72543rCan, RouteMatch::kExact},
    {"sub_ecu_hitachi_sh7058_can", kSubaruHitachiSh7058, RouteMatch::kExact},
    {"sub_tcu_cvt_hitachi_m32r_can", kSubaruTcuCvtHitachiM32rCan},
    {"sub_tcu_cvt_mitsu_mh8111_can", kSubaruTcuCvtMitsuMh8111Can},
    {"sub_tcu_cvt_mitsu_mh8104_can", kSubaruTcuCvtMitsuMh8104Can},
    {"sub_ecu_denso_1n83m_1_5m_can", kSubaruDenso1n83m15mCan},
    {"sub_ecu_denso_sh72531_can", kSubaruDensoSh72531Can},
    {"sub_ecu_denso_sh72543_can_diesel", kSubaruDensoSh72543CanDiesel},
    // kRoutes is matched by starts_with; "sub_ecu_denso_1n83m_1_5m_can" and
    // "sub_ecu_denso_1n83m_4m_can" are not prefixes of one another, so the
    // order of these two entries relative to each other does not matter.
    {"sub_ecu_denso_1n83m_4m_can", kSubaruDenso1n83m4mCan},
});

} // namespace

std::optional<bytes::Bytes> PortableImageForOperation(FlashOperation operation, bytes::ByteView rom)
{
    if (operation == FlashOperation::kRead)
    {
        return std::nullopt;
    }
    return bytes::Bytes(rom.begin(), rom.end());
}

std::unique_ptr<FlashWorkflow> FlashWorkflowFactory::TryCreate(FlashWorkflowRequest request)
{
    const auto route = std::ranges::find_if(kRoutes,
                                            [&request](const Route& candidate)
                                            {
                                                return candidate.match == RouteMatch::kExact
                                                           ? request.protocol.name == candidate.pattern
                                                           : request.protocol.name.starts_with(candidate.pattern);
                                            });
    if (route == kRoutes.end())
    {
        return nullptr;
    }

    switch (route->kind)
    {
    case kColt:
        return std::make_unique<ColtWorkflow>(std::move(request));
    case kEeprom:
        return std::make_unique<EepromWorkflow>(std::move(request));
    case kSubaruMitsuM32rKline:
        return std::make_unique<SubaruMitsuM32rKlineWorkflow>(std::move(request));
    case kSubaruHitachiM32rKline:
        return std::make_unique<SubaruHitachiM32rKlineWorkflow>(std::move(request));
    case kSubaruDensoMc68hc16y502:
        return std::make_unique<SubaruDensoMc68hc16y5_02Workflow>(std::move(request));
    case kSubaruDensoSh705502:
        return std::make_unique<SubaruDensoSh7055_02Workflow>(std::move(request));
    case kSubaruDensoSh705xDensoCan:
        return std::make_unique<SubaruDensoSh705xDensoCanWorkflow>(std::move(request));
    case kSubaruTcuDensoSh705xCan:
        return std::make_unique<SubaruTcuDensoSh705xCanWorkflow>(std::move(request));
    case kSubaruDensoSh7058Can:
        return std::make_unique<SubaruDensoSh7058CanWorkflow>(std::move(request));
    case kSubaruDensoSh7058CanDiesel:
        return std::make_unique<SubaruDensoSh7058CanDieselWorkflow>(std::move(request));
    case kSubaruHitachiM32rCan:
        return std::make_unique<SubaruHitachiM32rCanWorkflow>(std::move(request));
    case kSubaruTcuHitachiM32rKline:
        return std::make_unique<SubaruTcuHitachiM32rKlineWorkflow>(std::move(request));
    case kSubaruUnisiaJecs:
        return std::make_unique<SubaruUnisiaJecsWorkflow>(std::move(request));
    case kSubaruHitachiSh72543rCan:
        return std::make_unique<SubaruHitachiSh72543rCanWorkflow>(std::move(request));
    case kSubaruHitachiSh7058:
        if (request.operation == FlashOperation::kRead)
        {
            return std::make_unique<SubaruHitachiSh7058KlineWorkflow>(std::move(request));
        }
        return std::make_unique<SubaruHitachiSh7058CanWorkflow>(std::move(request));
    case kSubaruTcuHitachiM32rCan:
        return std::make_unique<SubaruTcuHitachiM32rCanWorkflow>(std::move(request));
    case kSubaruTcuCvtHitachiM32rCan:
        return std::make_unique<SubaruTcuCvtHitachiM32rCanWorkflow>(std::move(request));
    case kSubaruTcuCvtMitsuMh8111Can:
        return std::make_unique<SubaruTcuCvtMitsuMh8111CanWorkflow>(std::move(request));
    case kSubaruTcuCvtMitsuMh8104Can:
        return std::make_unique<SubaruTcuCvtMitsuMh8104CanWorkflow>(std::move(request));
    case kSubaruDenso1n83m15mCan:
        return std::make_unique<SubaruDenso1n83m_1_5mCanWorkflow>(std::move(request));
    case kSubaruDensoSh72531Can:
        return std::make_unique<SubaruDensoSh72531CanWorkflow>(std::move(request));
    case kSubaruDensoSh72543CanDiesel:
        return std::make_unique<SubaruDensoSh72543CanDieselWorkflow>(std::move(request));
    case kSubaruDenso1n83m4mCan:
        return std::make_unique<SubaruDenso1n83m_4mCanWorkflow>(std::move(request));
    case kSubaruDensoSh705xKline:
        return std::make_unique<SubaruDensoSh705xKlineWorkflow>(std::move(request));
    case kSubaruDensoMc68hc16y502Bdm:
        return std::make_unique<SubaruDensoMc68hc16y5_02BdmWorkflow>(std::move(request));
    case kSubaruUnisiaJecsM32rKline:
        return std::make_unique<SubaruUnisiaJecsM32rKlineWorkflow>(std::move(request));
    case kSubaruUnisiaJecsM32rBootMode:
        return std::make_unique<SubaruUnisiaJecsM32rBootModeWorkflow>(std::move(request));
    case kUnrouted:
        return nullptr;
    }
    assert(false);
    return nullptr;
}

} // namespace fastecu::flash
