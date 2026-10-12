#pragma once

#include "src/backend/flash/flash_transport_source.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/flash/testing/scripted_mixed_can_flash_transport.h"

#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace fastecu::flash
{

class ScriptedFlashTransportSource final : public IFlashTransportSource
{
  public:
    explicit ScriptedFlashTransportSource(bool supplies_programming_voltage = false)
        : supplies_programming_voltage_(supplies_programming_voltage)
    {
    }

    // Preloads must be non-null and unopened. Observers remain valid only
    // while this source or the returned transport owner keeps the object alive.
    void QueueKline(std::unique_ptr<ScriptedKlineFlashTransport> transport)
    {
        kline_.push_back(std::move(transport));
    }

    void QueueCan(std::unique_ptr<ScriptedCanFlashTransport> transport)
    {
        can_.push_back(std::move(transport));
    }

    void QueueMixedCan(std::unique_ptr<ScriptedMixedCanFlashTransport> transport)
    {
        mixed_can_.push_back(std::move(transport));
    }

    std::unique_ptr<IKlineFlashTransport> MakeKline() override
    {
        requested_kinds_.push_back(TransportKind::kKline);
        if (kline_.empty())
        {
            return std::make_unique<ScriptedKlineFlashTransport>();
        }
        auto transport = std::move(kline_.front());
        kline_.pop_front();
        return transport;
    }

    std::unique_ptr<ICanFlashTransport> MakeCan() override
    {
        requested_kinds_.push_back(TransportKind::kCanIso15765);
        if (can_.empty())
        {
            return std::make_unique<ScriptedCanFlashTransport>();
        }
        auto transport = std::move(can_.front());
        can_.pop_front();
        return transport;
    }

    std::unique_ptr<IMixedCanFlashTransport> MakeMixedCan() override
    {
        requested_kinds_.push_back(TransportKind::kCanRawIso15765);
        if (mixed_can_.empty())
        {
            return std::make_unique<ScriptedMixedCanFlashTransport>();
        }
        auto transport = std::move(mixed_can_.front());
        mixed_can_.pop_front();
        return transport;
    }

    bool SuppliesProgrammingVoltage() const override
    {
        return supplies_programming_voltage_;
    }

    const std::vector<TransportKind>& RequestedKinds() const
    {
        return requested_kinds_;
    }

  private:
    bool supplies_programming_voltage_;
    std::deque<std::unique_ptr<ScriptedKlineFlashTransport>> kline_;
    std::deque<std::unique_ptr<ScriptedCanFlashTransport>> can_;
    std::deque<std::unique_ptr<ScriptedMixedCanFlashTransport>> mixed_can_;
    std::vector<TransportKind> requested_kinds_;
};

} // namespace fastecu::flash
