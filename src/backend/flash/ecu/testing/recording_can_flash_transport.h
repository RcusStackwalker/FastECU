#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash
{

// This header holds two types the BEEF-dialect CAN executor suites share:
// RecordingCanFlashTransport, the recording decorator, and
// PhaseCancellingEventSink, an event sink that cancels partway through a
// named phase.
//
// RecordingCanFlashTransport wraps a ScriptedCanFlashTransport and adds the
// injection points those suites need: each is inert unless set, so one type
// serves all three families.
//
// This records lifecycle in its own `lifecycle` vector, which is NOT the same
// as ScriptedCanFlashTransport::lifecycle_calls_: reset_connection() below
// returns `reset_result` without delegating, so the scripted transport never
// sees a reset. Assert against this vector, not the scripted one.
class RecordingCanFlashTransport final : public ICanFlashTransport
{
  public:
    Status ResetConnection() override
    {
        lifecycle.push_back("reset_connection");
        if (timeline != nullptr)
        {
            timeline->push_back("reset_connection");
        }
        Status result = reset_result;
        if (result.has_value() && cancellation_on_reset != nullptr)
        {
            cancellation_on_reset->SetCancelled(true);
        }
        return result;
    }

    Status Configure(const Iso15765Config& config) override
    {
        lifecycle.push_back("configure");
        if (timeline != nullptr)
        {
            timeline->push_back("configure");
        }
        Status result = scripted.Configure(config);
        if (result.has_value() && cancellation_on_configure != nullptr)
        {
            cancellation_on_configure->SetCancelled(true);
        }
        return result;
    }

    Status Open() override
    {
        lifecycle.push_back("open");
        if (timeline != nullptr)
        {
            timeline->push_back("open");
        }
        Status result = scripted.Open();
        if (result.has_value() && cancellation_on_open != nullptr)
        {
            cancellation_on_open->SetCancelled(true);
        }
        return result;
    }

    Status Close() override
    {
        lifecycle.push_back("close");
        if (timeline != nullptr)
        {
            timeline->push_back("close");
        }
        return scripted.Close();
    }

    void RequestUnblock() noexcept override
    {
        scripted.RequestUnblock();
    }

    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        writes.emplace_back(data.begin(), data.end());
        Status result = scripted.Write(data, cancellation);
        if (result.has_value() && cancellation_to_trigger != nullptr && !cancel_prefix.empty() &&
            data.size() >= cancel_prefix.size() && std::equal(cancel_prefix.begin(), cancel_prefix.end(), data.begin()))
        {
            cancellation_to_trigger->SetCancelled(true);
        }
        return result;
    }

    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        read_timeouts.push_back(timeout);
        Result<std::optional<bytes::Bytes>> result = scripted.Read(timeout, cancellation);
        ++read_count;
        if (result.has_value() && cancellation_to_trigger != nullptr && cancel_after_read_count.has_value() &&
            read_count == *cancel_after_read_count)
        {
            cancellation_to_trigger->SetCancelled(true);
        }
        return result;
    }

    ScriptedCanFlashTransport scripted;
    Status reset_result;
    std::vector<std::string> lifecycle;
    std::vector<bytes::Bytes> writes;
    std::vector<std::chrono::milliseconds> read_timeouts;
    FakeCancellationToken *cancellation_to_trigger = nullptr;
    FakeCancellationToken *cancellation_on_reset = nullptr;
    FakeCancellationToken *cancellation_on_configure = nullptr;
    FakeCancellationToken *cancellation_on_open = nullptr;
    bytes::Bytes cancel_prefix;
    std::optional<std::size_t> cancel_after_read_count;
    std::size_t read_count{};
    std::vector<std::string> *timeline = nullptr;
};

// Cancels its bound token when a specific phase reaches a specific done count.
// Byte-identical in all three BEEF-dialect CAN executor suites before extraction
// (the token type changes from ToggleCancellation by Task 7's substitution).
class PhaseCancellingEventSink final : public RecordingEventSink
{
  public:
    PhaseCancellingEventSink(FakeCancellationToken& cancellation, std::string phase, int done)
        : cancellation_(cancellation), phase_(std::move(phase)), done_(done)
    {
    }

    void PhaseProgress(const PhaseProgressEvent& event) override
    {
        RecordingEventSink::PhaseProgress(event);
        if (event.phase_name == phase_ && event.done == done_)
        {
            cancellation_.SetCancelled(true);
        }
    }

  private:
    FakeCancellationToken& cancellation_;
    std::string phase_;
    int done_;
};

} // namespace fastecu::flash
