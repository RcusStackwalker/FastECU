#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
namespace
{

class LegacyBoundAttempt final : public BoundFlashAttempt
{
  public:
    LegacyBoundAttempt(FlashPlan plan, std::unique_ptr<IFlashExecutor> executor,
                       std::unique_ptr<IFlashTransport> transport)
        : plan_(std::move(plan)), executor_(std::move(executor)), transport_(std::move(transport))
    {
    }

    Result<FlashExecutionResult> run(IClock& clock, const ICancellationToken& cancellation, IEventSink& events) override
    {
        // No setup and no close: an unmigrated executor still does its own.
        return executor_->execute(plan_, *transport_, clock, cancellation, events);
    }

    void request_unblock() noexcept override
    {
        transport_->request_unblock();
    }

  private:
    FlashPlan plan_;
    std::unique_ptr<IFlashExecutor> executor_;
    std::unique_ptr<IFlashTransport> transport_;
};

} // namespace

Status check_family(const FlashPlan& plan, FlashFamily expected_family)
{
    if (plan.family() != expected_family)
    {
        return fail(ErrorKind::InvalidConfig, "plan family does not match this executor");
    }
    return {};
}

Status check_family_transport_match(const FlashPlan& plan, FlashFamily expected_family,
                                    TransportKind expected_transport)
{
    if (plan.family() != expected_family)
    {
        return fail(ErrorKind::InvalidConfig, "plan family does not match this executor");
    }
    if (plan.transport() != expected_transport)
    {
        return fail(ErrorKind::InvalidConfig, "plan transport does not match this executor");
    }
    return {};
}

std::unique_ptr<BoundFlashAttempt> bind_legacy_flash_attempt(FlashPlan plan, std::unique_ptr<IFlashExecutor> executor,
                                                             std::unique_ptr<IFlashTransport> transport)
{
    return std::make_unique<LegacyBoundAttempt>(std::move(plan), std::move(executor), std::move(transport));
}

Result<ICanFlashTransport *> open_can_iso15765_transport(IFlashTransport& transport, const Iso15765Config& config)
{
    auto *can = dynamic_cast<ICanFlashTransport *>(&transport);
    if (can == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "transport does not implement ICanFlashTransport");
    }
    if (const Status configured = can->configure(config); !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    if (const Status opened = can->open(); !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    return can;
}

} // namespace fastecu::flash
