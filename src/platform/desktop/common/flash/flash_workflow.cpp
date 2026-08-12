#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <array>
#include <cassert>
#include <string_view>

#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"
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
        SubaruHitachiM32rKline
    };

    std::string_view prefix;
    Kind kind;
};

constexpr std::array<Route, 9> kRoutes{{
    {"sub_ecu_hitachi_m32r_kline", Route::Kind::SubaruHitachiM32rKline},
    {"sub_ecu_mitsu_m32r_kline", Route::Kind::SubaruMitsuM32rKline},
    {"mitsu_ecu_m32r_can", Route::Kind::Colt},
    {"sub_ecu_eeprom_denso_sh7055_kline", Route::Kind::Eeprom},
    {"sub_ecu_eeprom_denso_sh7058_kline", Route::Kind::Eeprom},
    {"sub_ecu_eeprom_denso_sh7055_densocan", Route::Kind::Eeprom},
    {"sub_ecu_eeprom_denso_sh7058_densocan", Route::Kind::Eeprom},
    {"sub_ecu_eeprom_denso_sh7058_can_diesel", Route::Kind::Eeprom},
    {"sub_ecu_eeprom_denso_sh7058_can", Route::Kind::Eeprom},
}};

} // namespace

std::unique_ptr<FlashWorkflow> FlashWorkflowFactory::tryCreate(FlashWorkflowRequest request)
{
    for (const Route& route : kRoutes)
    {
        if (request.protocol.starts_with(route.prefix))
        {
            if (route.kind == Route::Kind::Colt)
            {
                return std::make_unique<ColtWorkflow>(std::move(request));
            }
            if (route.kind == Route::Kind::SubaruMitsuM32rKline)
            {
                return std::make_unique<SubaruM32rKlineWorkflow>(std::move(request), false);
            }
            if (route.kind == Route::Kind::SubaruHitachiM32rKline)
            {
                return std::make_unique<SubaruM32rKlineWorkflow>(std::move(request), true);
            }
            return std::make_unique<EepromWorkflow>(std::move(request));
        }
    }
    return nullptr;
}

} // namespace fastecu::flash
