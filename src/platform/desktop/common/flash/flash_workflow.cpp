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
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh7055_02_plan.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_plan.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.h"
#include "src/backend/flash/eeprom/eeprom_read_plan.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_kline_flash_transport.h"

namespace fastecu::flash
{
namespace
{

FlashCompletedStep completed(FlashWorkflowOutcome outcome,
                             std::optional<bytes::Bytes> bytes = std::nullopt,
                             std::optional<std::string> rom_id = std::nullopt)
{
    return {outcome, std::move(bytes), std::move(rom_id)};
}

Result<std::uint32_t> parseKernelStartAddress(std::string_view kernel_addr)
{
    const auto parsed = definition::parse_hex_value(kernel_addr);
    if (!parsed.has_value() || *parsed > std::numeric_limits<std::uint32_t>::max())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("kernel_addr did not parse as a 32-bit address: '{}'",
                                kernel_addr));
    }
    return static_cast<std::uint32_t>(*parsed);
}

Result<config::ProtocolEntry> resolveProtocol(const config::ConfigPaths& paths,
                                              std::string_view protocol_name,
                                              IFileRepository& repository)
{
    Result<config::ProtocolCatalog> protocols = config::load_protocol_catalog(paths, repository);
    if (!protocols.has_value())
    {
        return std::unexpected(protocols.error());
    }
    const auto entry = std::ranges::find(*protocols, protocol_name,
                                         &config::ProtocolEntry::protocol_name);
    if (entry == protocols->end())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("protocol '{}' is absent from the <protocols> section",
                                protocol_name));
    }
    return *entry;
}

Result<KernelImage> resolveKernel(const FlashWorkflowRequest& request,
                                  IFileRepository& repository)
{
    Result<config::ProtocolEntry> entry =
        resolveProtocol(request.paths, request.protocol, repository);
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
    return KernelImage{.id = request.protocol + "-kernel",
                       .load_address = *load_address,
                       .bytes = std::move(*kernel_bytes)};
}

std::optional<bytes::Bytes> normalizeMc68Image(std::optional<bytes::Bytes> image,
                                               std::string_view mcu_name)
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
        physical_size = std::max(physical_size,
                                 static_cast<std::size_t>(block.start) + block.len);
    }
    if (image->size() != physical_size)
    {
        return image;
    }

    bytes::Bytes packed;
    packed.reserve(device->romsize);
    std::size_t packed_remaining = device->romsize;
    for (unsigned block_no = 0;
         block_no < device->numblocks && packed_remaining > 0; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        const std::size_t block_bytes = std::min<std::size_t>(block.len, packed_remaining);
        packed.insert(packed.end(), image->begin() + block.start,
                      image->begin() + block.start + block_bytes);
        packed_remaining -= block_bytes;
    }
    return packed;
}

class SubaruM32rKlineWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruM32rKlineWorkflow(FlashWorkflowRequest request, bool hitachi)
        : request_(std::move(request)), hitachi_(hitachi),
          plan_(hitachi_ ? build_subaru_hitachi_m32r_kline_plan(
                               request_.operation, request_.protocol, request_.mcu,
                               std::move(request_.image))
                         : build_subaru_mitsu_m32r_kline_plan(
                               request_.operation, request_.protocol, request_.mcu,
                               std::move(request_.image)))
    {
    }
    FlashWorkflowStep next() override
    {
        if (!plan_)
        {
            return FlashFailureStep{plan_.error()};
        }
        if (failure_)
        {
            return FlashFailureStep{std::move(*failure_)};
        }
        if (terminal_)
        {
            return completed(outcome_, std::move(bytes_), std::move(rom_id_));
        }
        if (!begun_)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (!attempted_)
        {
            attempted_ = true;
            std::unique_ptr<IFlashExecutor> executor = hitachi_
                                                           ? std::unique_ptr<IFlashExecutor>(std::make_unique<SubaruHitachiM32rKlineExecutor>())
                                                           : std::unique_ptr<IFlashExecutor>(std::make_unique<SubaruMitsuM32rKlineExecutor>());
            return FlashAttempt{std::move(*plan_), std::move(executor),
                                std::make_unique<DesktopKlineFlashTransport>(request_.serial),
                                std::make_unique<QtClock>()};
        }
        return completed(outcome_, std::move(bytes_), std::move(rom_id_));
    }
    void submit(FlashPromptResponse response) override
    {
        begun_ = true;
        if (response != FlashPromptResponse::Accept)
        {
            terminal_ = true;
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
    }
    void submit(FlashAttemptResult result) override
    {
        terminal_ = true;
        if (result.success)
        {
            outcome_ = FlashWorkflowOutcome::Succeeded;
            bytes_ = std::move(result.read_bytes);
            rom_id_ = std::move(result.rom_id);
        }
        else if (result.error_kind == ErrorKind::Cancelled)
        {
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
        else
        {
            outcome_ = FlashWorkflowOutcome::Failed;
            failure_ = Error{result.error_kind, std::move(result.error_detail)};
        }
    }

  private:
    FlashWorkflowRequest request_;
    bool hitachi_;
    Result<FlashPlan> plan_;
    bool begun_ = false;
    bool attempted_ = false;
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::Failed;
    std::optional<bytes::Bytes> bytes_;
    std::optional<std::string> rom_id_;
    std::optional<Error> failure_;
};

class SubaruDensoMc68hc16y5_02Workflow final : public FlashWorkflow
{
  public:
    explicit SubaruDensoMc68hc16y5_02Workflow(FlashWorkflowRequest request)
        : request_(std::move(request))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!plan_.has_value())
        {
            // Desktop FullRomData is physically addressed after the legacy
            // calibration adapter inserts the 0x20000-0x27fff RAM/kernel hole.
            // Portable MC plans and executors use the packed flash-block image.
            request_.image = normalizeMc68Image(std::move(request_.image), request_.mcu);
            // Run the family builder first so recognized-but-unsupported
            // revision 04 is rejected by the plan even without a catalog.
            Result<FlashPlan> preflight = build_subaru_denso_mc68hc16y5_02_plan(
                request_.operation, request_.protocol, request_.mcu, request_.image,
                KernelImage{.id = request_.protocol + "-kernel",
                            .load_address = 0x20000,
                            .bytes = {0}});
            if (!preflight.has_value())
            {
                plan_ = std::unexpected(preflight.error());
            }
            else
            {
                QtFileRepository repository;
                Result<KernelImage> kernel = resolveKernel(request_, repository);
                if (!kernel.has_value())
                {
                    plan_ = std::unexpected(kernel.error());
                }
                else
                {
                    plan_ = build_subaru_denso_mc68hc16y5_02_plan(
                        request_.operation, request_.protocol, request_.mcu,
                        std::move(request_.image), std::move(*kernel));
                }
            }
        }
        if (!plan_->has_value())
        {
            return FlashFailureStep{plan_->error()};
        }
        if (failure_.has_value())
        {
            return FlashFailureStep{std::move(*failure_)};
        }
        if (terminal_)
        {
            return completed(outcome_, std::move(bytes_), std::move(rom_id_));
        }
        if (!begun_)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (!attempted_)
        {
            attempted_ = true;
            return FlashAttempt{std::move(**plan_),
                                std::make_unique<SubaruDensoMc68hc16y5_02Executor>(),
                                std::make_unique<DesktopKlineFlashTransport>(request_.serial),
                                std::make_unique<QtClock>()};
        }
        return completed(outcome_, std::move(bytes_), std::move(rom_id_));
    }

    void submit(FlashPromptResponse response) override
    {
        begun_ = true;
        if (response != FlashPromptResponse::Accept)
        {
            terminal_ = true;
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
    }

    void submit(FlashAttemptResult result) override
    {
        terminal_ = true;
        if (result.success)
        {
            outcome_ = FlashWorkflowOutcome::Succeeded;
            bytes_ = std::move(result.read_bytes);
            rom_id_ = std::move(result.rom_id);
        }
        else if (result.error_kind == ErrorKind::Cancelled)
        {
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
        else
        {
            outcome_ = FlashWorkflowOutcome::Failed;
            failure_ = Error{result.error_kind, std::move(result.error_detail)};
        }
    }

  private:
    FlashWorkflowRequest request_;
    std::optional<Result<FlashPlan>> plan_;
    bool begun_ = false;
    bool attempted_ = false;
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::Failed;
    std::optional<bytes::Bytes> bytes_;
    std::optional<std::string> rom_id_;
    std::optional<Error> failure_;
};

class SubaruDensoSh7055_02Workflow final : public FlashWorkflow
{
  public:
    explicit SubaruDensoSh7055_02Workflow(FlashWorkflowRequest request)
        : request_(std::move(request))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!plan_.has_value())
        {
            QtFileRepository repository;
            Result<KernelImage> kernel = resolveKernel(request_, repository);
            if (!kernel.has_value())
            {
                plan_ = std::unexpected(kernel.error());
            }
            else
            {
                plan_ = build_subaru_denso_sh7055_02_plan(
                    request_.operation, request_.protocol, request_.mcu,
                    std::move(request_.image), std::move(*kernel));
            }
        }
        if (!plan_->has_value())
        {
            return FlashFailureStep{plan_->error()};
        }
        if (failure_.has_value())
        {
            return FlashFailureStep{std::move(*failure_)};
        }
        if (terminal_)
        {
            return completed(outcome_, std::move(bytes_), std::move(rom_id_));
        }
        if (stage_ == 0)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        const auto confirmations = (*plan_)->confirmations();
        if (stage_ <= confirmations.size())
        {
            const ConfirmationSpec& confirmation = confirmations[stage_ - 1];
            return FlashPromptStep{FlashPromptKind::CycleIgnition,
                                   confirmation.arguments};
        }
        if (!attempted_)
        {
            attempted_ = true;
            return FlashAttempt{std::move(**plan_),
                                std::make_unique<SubaruDensoSh7055_02Executor>(),
                                std::make_unique<DesktopKlineFlashTransport>(request_.serial),
                                std::make_unique<QtClock>()};
        }
        return completed(outcome_, std::move(bytes_), std::move(rom_id_));
    }

    void submit(FlashPromptResponse response) override
    {
        if (response != FlashPromptResponse::Accept)
        {
            terminal_ = true;
            outcome_ = FlashWorkflowOutcome::Cancelled;
            return;
        }
        ++stage_;
    }

    void submit(FlashAttemptResult result) override
    {
        terminal_ = true;
        if (result.success)
        {
            outcome_ = FlashWorkflowOutcome::Succeeded;
            bytes_ = std::move(result.read_bytes);
            rom_id_ = std::move(result.rom_id);
        }
        else if (result.error_kind == ErrorKind::Cancelled)
        {
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
        else
        {
            outcome_ = FlashWorkflowOutcome::Failed;
            failure_ = Error{result.error_kind, std::move(result.error_detail)};
        }
    }

  private:
    FlashWorkflowRequest request_;
    std::optional<Result<FlashPlan>> plan_;
    std::size_t stage_ = 0;
    bool attempted_ = false;
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::Failed;
    std::optional<bytes::Bytes> bytes_;
    std::optional<std::string> rom_id_;
    std::optional<Error> failure_;
};

class ColtWorkflow final : public FlashWorkflow
{
  public:
    explicit ColtWorkflow(FlashWorkflowRequest request)
        : request_(std::move(request)),
          plan_(build_mitsu_colt_m32r_can_plan(request_.operation, request_.protocol,
                                               request_.mcu, std::move(request_.image)))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!plan_)
        {
            return FlashFailureStep{plan_.error()};
        }
        if (failure_)
        {
            return FlashFailureStep{std::move(*failure_)};
        }
        if (terminal_)
        {
            return completed(outcome_, std::move(accepted_));
        }
        if (stage_ == 0)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (const auto confirmations = plan_->confirmations(); stage_ <= confirmations.size())
        {
            const auto& spec = confirmations[stage_ - 1];
            return FlashPromptStep{
                spec.id == ConfirmationSpec::Id::EraseTrigger
                    ? FlashPromptKind::ColtEraseTrigger
                    : FlashPromptKind::ColtTopRegionBootstrap,
                spec.arguments};
        }
        if (!attempted_)
        {
            attempted_ = true;
            FlashPlan plan = std::move(*plan_);
            return FlashAttempt{std::move(plan),
                                std::make_unique<MitsuColtM32rCanExecutor>(),
                                std::make_unique<DesktopCanFlashTransport>(request_.serial),
                                std::make_unique<QtClock>()};
        }
        return completed(outcome_, std::move(accepted_));
    }

    void submit(FlashPromptResponse response) override
    {
        if (response != FlashPromptResponse::Accept)
        {
            terminal_ = true;
            outcome_ = FlashWorkflowOutcome::Cancelled;
            return;
        }
        ++stage_;
    }

    void submit(FlashAttemptResult result) override
    {
        terminal_ = true;
        if (result.success)
        {
            outcome_ = FlashWorkflowOutcome::Succeeded;
            accepted_ = std::move(result.read_bytes);
        }
        else if (result.error_kind == ErrorKind::Cancelled)
        {
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
        else
        {
            outcome_ = FlashWorkflowOutcome::Failed;
            failure_ = Error{result.error_kind, std::move(result.error_detail)};
        }
    }

  private:
    FlashWorkflowRequest request_;
    Result<FlashPlan> plan_;
    std::size_t stage_ = 0;
    bool attempted_ = false;
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::Failed;
    std::optional<bytes::Bytes> accepted_;
    std::optional<Error> failure_;
};

class SubaruHitachiM32rCanWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruHitachiM32rCanWorkflow(FlashWorkflowRequest request)
        : request_(std::move(request)),
          plan_(build_subaru_hitachi_m32r_can_plan(request_.operation, request_.protocol,
                                                   request_.mcu, std::move(request_.image)))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!plan_)
        {
            return FlashFailureStep{plan_.error()};
        }
        if (failure_)
        {
            return FlashFailureStep{std::move(*failure_)};
        }
        if (terminal_)
        {
            return completed(outcome_, std::move(accepted_));
        }
        if (!began_)
        {
            began_ = true;
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (!attempted_)
        {
            attempted_ = true;
            FlashPlan plan = std::move(*plan_);
            return FlashAttempt{std::move(plan), std::make_unique<SubaruHitachiM32rCanExecutor>(),
                                std::make_unique<DesktopCanFlashTransport>(request_.serial),
                                std::make_unique<QtClock>()};
        }
        return completed(outcome_, std::move(accepted_));
    }

    void submit(FlashPromptResponse response) override
    {
        if (response != FlashPromptResponse::Accept)
        {
            terminal_ = true;
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
    }

    void submit(FlashAttemptResult result) override
    {
        terminal_ = true;
        if (result.success)
        {
            outcome_ = FlashWorkflowOutcome::Succeeded;
            accepted_ = std::move(result.read_bytes);
        }
        else if (result.error_kind == ErrorKind::Cancelled)
        {
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
        else
        {
            outcome_ = FlashWorkflowOutcome::Failed;
            failure_ = Error{result.error_kind, std::move(result.error_detail)};
        }
    }

  private:
    FlashWorkflowRequest request_;
    Result<FlashPlan> plan_;
    bool began_ = false;
    bool attempted_ = false;
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::Failed;
    std::optional<bytes::Bytes> accepted_;
    std::optional<Error> failure_;
};

// Same ~50-line shape as SubaruHitachiM32rCanWorkflow above (kernel-free,
// no confirmations, single-attempt CAN executor) -- duplicated rather than
// parametrized, matching the brief's explicit allowance not to introduce a
// runtime branch inside one class for what are compile-time-distinct
// executor types.
class SubaruTcuCvtHitachiM32rCanWorkflow final : public FlashWorkflow
{
  public:
    explicit SubaruTcuCvtHitachiM32rCanWorkflow(FlashWorkflowRequest request)
        : request_(std::move(request)),
          plan_(build_subaru_tcu_cvt_hitachi_m32r_can_plan(request_.operation, request_.protocol,
                                                           request_.mcu, std::move(request_.image)))
    {
    }

    FlashWorkflowStep next() override
    {
        if (!plan_)
        {
            return FlashFailureStep{plan_.error()};
        }
        if (failure_)
        {
            return FlashFailureStep{std::move(*failure_)};
        }
        if (terminal_)
        {
            return completed(outcome_, std::move(accepted_));
        }
        if (!began_)
        {
            began_ = true;
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (!attempted_)
        {
            attempted_ = true;
            FlashPlan plan = std::move(*plan_);
            return FlashAttempt{std::move(plan),
                                std::make_unique<SubaruTcuCvtHitachiM32rCanExecutor>(),
                                std::make_unique<DesktopCanFlashTransport>(request_.serial),
                                std::make_unique<QtClock>()};
        }
        return completed(outcome_, std::move(accepted_));
    }

    void submit(FlashPromptResponse response) override
    {
        if (response != FlashPromptResponse::Accept)
        {
            terminal_ = true;
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
    }

    void submit(FlashAttemptResult result) override
    {
        terminal_ = true;
        if (result.success)
        {
            outcome_ = FlashWorkflowOutcome::Succeeded;
            accepted_ = std::move(result.read_bytes);
        }
        else if (result.error_kind == ErrorKind::Cancelled)
        {
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
        else
        {
            outcome_ = FlashWorkflowOutcome::Failed;
            failure_ = Error{result.error_kind, std::move(result.error_detail)};
        }
    }

  private:
    FlashWorkflowRequest request_;
    Result<FlashPlan> plan_;
    bool began_ = false;
    bool attempted_ = false;
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::Failed;
    std::optional<bytes::Bytes> accepted_;
    std::optional<Error> failure_;
};

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
            return FlashFailureStep{Error{ErrorKind::Unsupported,
                                          "EEPROM workflows support read operations only"}};
        }
        if (terminal_)
        {
            if (failure_)
            {
                return FlashFailureStep{std::move(*failure_)};
            }
            return completed(outcome_, std::move(accepted_));
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
        const TransportKind transport = plan->transport();
        std::unique_ptr<IFlashExecutor> executor;
        std::unique_ptr<IFlashTransport> adapter;
        if (transport == TransportKind::Kline)
        {
            executor = std::make_unique<DensoSh705xEepromKlineExecutor>();
            adapter = std::make_unique<DesktopKlineFlashTransport>(request_.serial);
        }
        else
        {
            executor = std::make_unique<DensoSh705xEepromCanExecutor>();
            adapter = std::make_unique<DesktopCanFlashTransport>(request_.serial);
        }
        return FlashAttempt{std::move(*plan), std::move(executor), std::move(adapter),
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
                terminal_ = true;
                outcome_ = FlashWorkflowOutcome::Cancelled;
            }
            return;
        }
        if (need_cycle_)
        {
            need_cycle_ = false;
            if (response != FlashPromptResponse::Accept)
            {
                terminal_ = true;
                outcome_ = FlashWorkflowOutcome::Cancelled;
            }
            return;
        }
        if (inspect_)
        {
            inspect_ = false;
            if (response == FlashPromptResponse::Save)
            {
                accepted_ = std::move(pending_);
                terminal_ = true;
                outcome_ = FlashWorkflowOutcome::Succeeded;
            }
            else if (mode_ == EepromReadMode::Mode4)
            {
                terminal_ = true;
                outcome_ = FlashWorkflowOutcome::Discarded;
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
            terminal_ = true;
            outcome_ = FlashWorkflowOutcome::Cancelled;
        }
        else if (mode_ != EepromReadMode::Mode4)
        {
            advance();
        }
        else
        {
            terminal_ = true;
            outcome_ = FlashWorkflowOutcome::Failed;
            failure_ = Error{result.error_kind, std::move(result.error_detail)};
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
    bool terminal_ = false;
    FlashWorkflowOutcome outcome_ = FlashWorkflowOutcome::Failed;
    std::optional<bytes::Bytes> pending_;
    std::optional<bytes::Bytes> accepted_;
    std::optional<Error> failure_;
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
        SubaruHitachiM32rCan,
        SubaruTcuCvtHitachiM32rCan,
        Unrouted,
    };

    std::string_view prefix;
    Kind kind;
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
    // Reserve this longer prefix before the bare MC68 _02 row. BDM remains
    // on its legacy path and must not be swallowed by portable routing.
    {"sub_ecu_denso_mc68hc16y5_02_bdm", Unrouted},
    {"sub_ecu_denso_mc68hc16y5_02", SubaruDensoMc68hc16y5_02},
    {"sub_ecu_denso_mc68hc16y5_04", SubaruDensoMc68hc16y5_02},
    {"sub_ecu_denso_sh7055_02", SubaruDensoSh7055_02},
    {"sub_ecu_hitachi_m32r_can", SubaruHitachiM32rCan},
    {"sub_tcu_cvt_hitachi_m32r_can", SubaruTcuCvtHitachiM32rCan},
});

} // namespace

std::optional<bytes::Bytes> portableImageForOperation(FlashOperation operation,
                                                      bytes::ByteView rom)
{
    if (operation == FlashOperation::Read)
    {
        return std::nullopt;
    }
    return bytes::Bytes(rom.begin(), rom.end());
}

std::unique_ptr<FlashWorkflow> FlashWorkflowFactory::tryCreate(FlashWorkflowRequest request)
{
    const auto route = std::ranges::find_if(
        kRoutes, [&request](const Route& candidate)
        { return request.protocol.starts_with(candidate.prefix); });
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
        return std::make_unique<SubaruM32rKlineWorkflow>(std::move(request), false);
    case SubaruHitachiM32rKline:
        return std::make_unique<SubaruM32rKlineWorkflow>(std::move(request), true);
    case SubaruDensoMc68hc16y5_02:
        return std::make_unique<SubaruDensoMc68hc16y5_02Workflow>(std::move(request));
    case SubaruDensoSh7055_02:
        return std::make_unique<SubaruDensoSh7055_02Workflow>(std::move(request));
    case SubaruHitachiM32rCan:
        return std::make_unique<SubaruHitachiM32rCanWorkflow>(std::move(request));
    case SubaruTcuCvtHitachiM32rCan:
        return std::make_unique<SubaruTcuCvtHitachiM32rCanWorkflow>(std::move(request));
    case Unrouted:
        return nullptr;
    }
    assert(false);
    return nullptr;
}

} // namespace fastecu::flash
