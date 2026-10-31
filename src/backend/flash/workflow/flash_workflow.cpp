#include "src/backend/flash/workflow/flash_workflow.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <format>
#include <span>
#include <string_view>

#include "src/algorithms/memory/address.h"
#include "src/algorithms/memory/memory_image.h"
#include "src/backend/config/catalog.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/ecu/protocol_kernel.h"
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

    // The step of a finished workflow: its recorded failure, moved out, or
    // its completion. Empty while the workflow is still running.
    std::optional<FlashWorkflowStep> FinishedStep()
    {
        if (failure_.has_value())
        {
            return FlashFailureStep{std::move(*failure_)};
        }
        if (terminal_)
        {
            return CompletedStep();
        }
        return std::nullopt;
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

// How an attempt ended, as the "outcome" argument of the remove-VPP notice.
std::string AttemptOutcomeName(const FlashAttemptResult& result)
{
    if (result.success)
    {
        return "succeeded";
    }
    return result.error_kind == ErrorKind::kCancelled ? "cancelled" : "failed";
}

// The workflow's only way to bind an attempt: an executor that does not run
// plan admission through FamilyExecutor cannot reach the worker.
template <AdmittingExecutor Executor, typename Transport>
std::unique_ptr<BoundFlashAttempt> BindAdmittedAttempt(FlashPlan plan, std::unique_ptr<Executor> executor,
                                                       std::unique_ptr<Transport> transport)
{
    return BindFlashAttempt(std::move(plan), std::move(executor), std::move(transport));
}

// A new transport of `Transport`, one of the three flash transport
// interfaces, from the workflow's source.
template <typename Transport> std::unique_ptr<Transport> MakeTransport(IFlashTransportSource& source)
{
    if constexpr (std::same_as<Transport, IKlineFlashTransport>)
    {
        return source.MakeKline();
    }
    else if constexpr (std::same_as<Transport, ICanFlashTransport>)
    {
        return source.MakeCan();
    }
    else
    {
        static_assert(std::same_as<Transport, IMixedCanFlashTransport>);
        return source.MakeMixedCan();
    }
}

// The transport source answers whether the adapter supplies programming voltage
// (legacy write_mem()); one that does needs no operator prompt.
class SubaruUnisiaJecsM32rKlineWorkflow final : public FlashWorkflow
{
  public:
    SubaruUnisiaJecsM32rKlineWorkflow(FlashWorkflowRequest request, FlashWorkflowPorts ports)
        : request_(std::move(request)), ports_(std::move(ports)),
          plan_(BuildSubaruUnisiaJecsM32rKlinePlan(request_.operation, request_.protocol.name, request_.protocol.mcu,
                                                   std::move(request_.image),
                                                   ports_.transports->SuppliesProgrammingVoltage()))
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
        if (auto finished = outcome_.FinishedStep(); finished.has_value())
        {
            return *std::move(finished);
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
                                     BindAdmittedAttempt(std::move(*plan_),
                                                         std::make_unique<SubaruUnisiaJecsM32rKlineExecutor>(),
                                                         ports_.transports->MakeKline())};
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
            attempt_outcome_ = AttemptOutcomeName(result);
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
    FlashWorkflowPorts ports_; // Before plan_: its initializer asks the transport source.
    Result<FlashPlan> plan_;
    bool is_write_ = request_.operation == FlashOperation::kWrite;
    bool needs_vpp_ = false;
    Stage stage_ = Stage::kBegin;
    std::string attempt_outcome_;
    FlashAttemptOutcome outcome_;
};

// Read is the 6c-3 K-Line read. Write is two attempts with an operator step
// between them, where legacy reset the connection anyway: kernel upload,
// RemoveMod1, erase and program. Both plans are built before Begin so a missing
// kernel or a wrong-size image fails before any prompt.
class SubaruUnisiaJecsM32rBootModeWorkflow final : public FlashWorkflow
{
  public:
    SubaruUnisiaJecsM32rBootModeWorkflow(FlashWorkflowRequest request, FlashWorkflowPorts ports)
        : request_(std::move(request)), ports_(std::move(ports))
    {
    }

    FlashWorkflowStep Next() override
    {
        if (auto step = StepBeforeStage(); step.has_value())
        {
            return *std::move(step);
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
        notice_outcome_ = AttemptOutcomeName(result);
        stage_ = Stage::kNotice;
        outcome_.Record(std::move(result));
    }

  private:
    // A plan failure, the VPP notice, or the outcome of a finished attempt.
    std::optional<FlashWorkflowStep> StepBeforeStage()
    {
        if (!built_.has_value())
        {
            built_ = PrepareSubaruUnisiaJecsM32rBootModePlans(request_.operation, request_.protocol, request_.paths,
                                                              std::move(request_.image), *ports_.files);
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
        return outcome_.FinishedStep();
    }

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

    bool IsWrite() const
    {
        return request_.operation != FlashOperation::kRead;
    }

    // Reached only after StepBeforeStage() built the plans successfully.
    FlashWorkflowStep FirstAttempt()
    {
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
        return FlashWorkflowStep{
            std::in_place_type<FlashAttempt>,
            BindAdmittedAttempt(std::move(plan), std::move(executor), ports_.transports->MakeKline())};
    }

    FlashWorkflowRequest request_;
    FlashWorkflowPorts ports_;
    std::optional<Result<SubaruUnisiaJecsM32rBootModePlans>> built_;
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
// A family's plan preparation, given the parts of the request it reads. It
// consumes the ROM file. A family with its own preparation (Decision 6 of the
// flash family locality design) exports one; the others are adapted from
// their builder by PrepareWithoutKernel or PrepareWithProtocolKernel.
using FamilyPlanPreparation = Result<FlashPlan> (*)(FlashOperation, const config::ProtocolSpec&,
                                                    const config::ConfigPaths&, std::optional<bytes::Bytes>,
                                                    IFileRepository&);

// Plan preparation for a kernel-free family: the builder needs no I/O.
template <KernelFreePlanBuilder Build>
Result<FlashPlan> PrepareWithoutKernel(FlashOperation operation, const config::ProtocolSpec& protocol,
                                       const config::ConfigPaths& /*paths*/, std::optional<bytes::Bytes> rom_file,
                                       IFileRepository& /*files*/)
{
    return Build(operation, protocol.name, protocol.mcu, std::move(rom_file));
}

// The control flow shared by every family whose operation is one attempt:
// preflight, Begin, the plan's confirmations in order, the attempt, then its
// result. Accepting a prompt advances to the next one; any other response
// cancels before the attempt is made. Families differ only in the executor,
// the transport interface bound to it (bind_flash_attempt rejects a mismatch at
// compile time), and how the plan is prepared.
template <typename Executor, typename Transport, FamilyPlanPreparation Prepare>
class SingleAttemptFlashWorkflow final : public FlashWorkflow
{
  public:
    SingleAttemptFlashWorkflow(FlashWorkflowRequest request, FlashWorkflowPorts ports)
        : request_(std::move(request)), ports_(std::move(ports))
    {
    }

    FlashWorkflowStep Next() override
    {
        Result<FlashPlan>& plan = Plan();
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
        if (auto finished = outcome_.FinishedStep(); finished.has_value())
        {
            return *std::move(finished);
        }
        if (const std::vector<FlashPromptStep>& prompts = **prompts_; accepted_ < prompts.size())
        {
            return prompts[accepted_];
        }
        if (!attempted_)
        {
            attempted_ = true;
            return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                     BindAdmittedAttempt(std::move(*plan), std::make_unique<Executor>(),
                                                         MakeTransport<Transport>(*ports_.transports))};
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
    // Prepares the plan on the first Next(), before Begin, and keeps the
    // result: a preparation failure is reported before any prompt, and the
    // attempt carries the snapshot taken then even if the files change
    // afterward.
    Result<FlashPlan>& Plan()
    {
        if (!plan_.has_value())
        {
            plan_ = Prepare(request_.operation, request_.protocol, request_.paths, std::move(request_.image),
                            *ports_.files);
        }
        return *plan_;
    }

    FlashWorkflowRequest request_;
    FlashWorkflowPorts ports_;
    std::optional<Result<FlashPlan>> plan_;
    std::optional<Result<std::vector<FlashPromptStep>>> prompts_;
    std::size_t accepted_ = 0;
    bool attempted_ = false;
    FlashAttemptOutcome outcome_;
};

template <typename Executor, typename Transport, KernelFreePlanBuilder Build>
using KernelFreeWorkflow = SingleAttemptFlashWorkflow<Executor, Transport, &PrepareWithoutKernel<Build>>;

template <typename Executor, KernelFreePlanBuilder Build>
using KernelFreeCanWorkflow = KernelFreeWorkflow<Executor, ICanFlashTransport, Build>;

template <typename Executor, KernelFreePlanBuilder Build>
using KernelFreeKlineWorkflow = KernelFreeWorkflow<Executor, IKlineFlashTransport, Build>;

template <typename Executor, typename Transport, KernelBackedPlanBuilder Build>
using KernelBackedWorkflow = SingleAttemptFlashWorkflow<Executor, Transport, &PrepareWithProtocolKernel<Build>>;

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
    KernelBackedWorkflow<SubaruDensoSh705xDensoCanExecutor, IMixedCanFlashTransport,
                         &BuildSubaruDensoSh705xDensocanPlan>;
using SubaruTcuDensoSh705xCanWorkflow =
    KernelBackedWorkflow<SubaruTcuDensoSh705xCanExecutor, ICanFlashTransport, &BuildSubaruTcuDensoSh705xCanPlan>;
using SubaruDensoSh7058CanWorkflow =
    KernelBackedWorkflow<SubaruDensoSh7058CanExecutor, ICanFlashTransport, &BuildSubaruDensoSh7058CanPlan>;
using SubaruDensoSh7058CanDieselWorkflow =
    KernelBackedWorkflow<SubaruDensoSh7058CanDieselExecutor, ICanFlashTransport, &BuildSubaruDensoSh7058CanDieselPlan>;
// The sequence is the kernel resolved on the first step, the shared
// Begin prompt -- the legacy dialog's only prompt, "Turn ignition ON" -- then
// the attempt.
using SubaruDensoSh705xKlineWorkflow =
    KernelBackedWorkflow<SubaruDensoSh705xKlineExecutor, IKlineFlashTransport, &BuildSubaruDensoSh705xKlinePlan>;
// SH7055_02 plans carry exactly one CycleIgnition confirmation.
using SubaruDensoSh7055_02Workflow =
    KernelBackedWorkflow<SubaruDensoSh7055_02Executor, IKlineFlashTransport, &BuildSubaruDensoSh705502Plan>;
using SubaruDensoMc68hc16y5_02Workflow =
    SingleAttemptFlashWorkflow<SubaruDensoMc68hc16y5_02Executor, IKlineFlashTransport,
                               &PrepareSubaruDensoMc68hc16y502Plan>;
// Hitachi SH7058 Read runs over K-Line and Write over CAN: the plan builder
// chooses the transport by operation and the factory the matching executor.
// Read plans carry StartKlineRead; write plans carry nothing.
using SubaruHitachiSh7058KlineWorkflow =
    KernelFreeKlineWorkflow<SubaruHitachiSh7058KlineExecutor, &BuildSubaruHitachiSh7058Plan>;
using SubaruHitachiSh7058CanWorkflow =
    KernelFreeCanWorkflow<SubaruHitachiSh7058CanExecutor, &BuildSubaruHitachiSh7058Plan>;
// Write plans carry KernelBootstrap; read plans carry nothing.
using SubaruDensoMc68hc16y5_02BdmWorkflow =
    SingleAttemptFlashWorkflow<SubaruDensoMc68hc16y5_02BdmExecutor, IKlineFlashTransport,
                               &PrepareSubaruDensoMc68hc16y502BdmPlan>;

// A Denso SH705x EEPROM read: Begin, then one attempt per mode (2, 3, 4)
// until the operator saves one. The route picks the executor, and with it the
// family the plan is built for and the transport it is bound to.
template <typename Executor, typename Transport> class EepromWorkflow final : public FlashWorkflow
{
  public:
    EepromWorkflow(FlashWorkflowRequest request, FlashWorkflowPorts ports)
        : request_(std::move(request)), ports_(std::move(ports))
    {
    }

    FlashWorkflowStep Next() override
    {
        if (request_.operation != FlashOperation::kRead)
        {
            return FlashFailureStep{Error{ErrorKind::kUnsupported, "EEPROM workflows support read operations only"}};
        }
        if (auto finished = outcome_.FinishedStep(); finished.has_value())
        {
            return *std::move(finished);
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

        auto plan = BuildEepromReadPlan(FamilyTraits<typename Executor::PlanType>::kFamily, request_.paths,
                                        request_.protocol, mode_, *ports_.files);
        if (!plan)
        {
            return FlashFailureStep{plan.error()};
        }
        return FlashWorkflowStep{std::in_place_type<FlashAttempt>,
                                 BindAdmittedAttempt(std::move(*plan), std::make_unique<Executor>(),
                                                     MakeTransport<Transport>(*ports_.transports))};
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
    FlashWorkflowPorts ports_;
    EepromReadMode mode_ = EepromReadMode::kMode2;
    bool begun_ = false;
    bool need_cycle_ = false;
    bool inspect_ = false;
    std::optional<bytes::Bytes> pending_;
    FlashAttemptOutcome outcome_;
};

using EepromKlineWorkflow = EepromWorkflow<DensoSh705xEepromKlineExecutor, IKlineFlashTransport>;
using EepromCanWorkflow = EepromWorkflow<DensoSh705xEepromCanExecutor, ICanFlashTransport>;

using CreateWorkflow = std::unique_ptr<FlashWorkflow> (*)(FlashWorkflowRequest, FlashWorkflowPorts);

template <typename Workflow> std::unique_ptr<FlashWorkflow> Make(FlashWorkflowRequest request, FlashWorkflowPorts ports)
{
    return std::make_unique<Workflow>(std::move(request), std::move(ports));
}

// SH7058 reads over K-Line and writes over CAN.
std::unique_ptr<FlashWorkflow> MakeSubaruHitachiSh7058(FlashWorkflowRequest request, FlashWorkflowPorts ports)
{
    return request.operation == FlashOperation::kRead
               ? Make<SubaruHitachiSh7058KlineWorkflow>(std::move(request), std::move(ports))
               : Make<SubaruHitachiSh7058CanWorkflow>(std::move(request), std::move(ports));
}

struct FlashRoute
{
    std::span<const std::string_view> protocols;
    CreateWorkflow create;
};

// Each family owns the exact protocol ids routed to it; an id is never matched
// by prefix, so the order of these rows does not matter.
std::span<const FlashRoute> FlashRoutes()
{
    static const auto kFlashRoutes = std::to_array<FlashRoute>({
        {MitsuColtM32rCanProtocols(), &Make<ColtWorkflow>},
        {DensoSh705xEepromKlineProtocols(), &Make<EepromKlineWorkflow>},
        {DensoSh705xEepromCanProtocols(), &Make<EepromCanWorkflow>},
        {SubaruMitsuM32rKlineProtocols(), &Make<SubaruMitsuM32rKlineWorkflow>},
        {SubaruHitachiM32rKlineProtocols(), &Make<SubaruHitachiM32rKlineWorkflow>},
        {SubaruDensoMc68hc16y502Protocols(), &Make<SubaruDensoMc68hc16y5_02Workflow>},
        {SubaruDensoSh705502Protocols(), &Make<SubaruDensoSh7055_02Workflow>},
        {SubaruDensoSh705xDensoCanProtocols(), &Make<SubaruDensoSh705xDensoCanWorkflow>},
        {SubaruTcuDensoSh705xCanProtocols(), &Make<SubaruTcuDensoSh705xCanWorkflow>},
        {SubaruDensoSh7058CanProtocols(), &Make<SubaruDensoSh7058CanWorkflow>},
        {SubaruDensoSh7058CanDieselProtocols(), &Make<SubaruDensoSh7058CanDieselWorkflow>},
        {SubaruHitachiM32rCanProtocols(), &Make<SubaruHitachiM32rCanWorkflow>},
        {SubaruTcuHitachiM32rKlineProtocols(), &Make<SubaruTcuHitachiM32rKlineWorkflow>},
        {SubaruUnisiaJecsProtocols(), &Make<SubaruUnisiaJecsWorkflow>},
        {SubaruTcuHitachiM32rCanProtocols(), &Make<SubaruTcuHitachiM32rCanWorkflow>},
        {SubaruHitachiSh72543rCanProtocols(), &Make<SubaruHitachiSh72543rCanWorkflow>},
        {SubaruHitachiSh7058Protocols(), &MakeSubaruHitachiSh7058},
        {SubaruTcuCvtHitachiM32rCanProtocols(), &Make<SubaruTcuCvtHitachiM32rCanWorkflow>},
        {SubaruTcuCvtMitsuMh8111CanProtocols(), &Make<SubaruTcuCvtMitsuMh8111CanWorkflow>},
        {SubaruTcuCvtMitsuMh8104CanProtocols(), &Make<SubaruTcuCvtMitsuMh8104CanWorkflow>},
        {SubaruDenso1n83m15mCanProtocols(), &Make<SubaruDenso1n83m_1_5mCanWorkflow>},
        {SubaruDensoSh72531CanProtocols(), &Make<SubaruDensoSh72531CanWorkflow>},
        {SubaruDensoSh72543CanDieselProtocols(), &Make<SubaruDensoSh72543CanDieselWorkflow>},
        {SubaruDenso1n83m4mCanProtocols(), &Make<SubaruDenso1n83m_4mCanWorkflow>},
        {SubaruDensoSh705xKlineProtocols(), &Make<SubaruDensoSh705xKlineWorkflow>},
        {SubaruDensoMc68hc16y502BdmProtocols(), &Make<SubaruDensoMc68hc16y5_02BdmWorkflow>},
        {SubaruUnisiaJecsM32rKlineProtocols(), &Make<SubaruUnisiaJecsM32rKlineWorkflow>},
        {SubaruUnisiaJecsM32rBootModeProtocols(), &Make<SubaruUnisiaJecsM32rBootModeWorkflow>},
    });
    return kFlashRoutes;
}

} // namespace

std::optional<bytes::Bytes> PortableImageForOperation(FlashOperation operation, bytes::ByteView rom)
{
    if (operation == FlashOperation::kRead)
    {
        return std::nullopt;
    }
    return bytes::Bytes(rom.begin(), rom.end());
}

std::unique_ptr<FlashWorkflow> FlashWorkflowFactory::TryCreate(FlashWorkflowRequest request, FlashWorkflowPorts ports)
{
    const auto routes = FlashRoutes();
    const auto route =
        std::ranges::find_if(routes, [&request](const FlashRoute& candidate)
                             { return std::ranges::contains(candidate.protocols, request.protocol.name); });
    return route == routes.end() ? nullptr : route->create(std::move(request), std::move(ports));
}

std::vector<std::string_view> FlashWorkflowFactory::RoutedProtocolIds()
{
    std::vector<std::string_view> ids;
    for (const FlashRoute& route : FlashRoutes())
    {
        ids.insert(ids.end(), route.protocols.begin(), route.protocols.end());
    }
    return ids;
}

} // namespace fastecu::flash
