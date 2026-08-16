#pragma once

#include <algorithm>
#include <string_view>

#include "src/backend/ports/event_sink.h"

// Shared multi-phase progress reporting for this package's CAN executors
// (mitsu_colt_m32r, subaru_hitachi_m32r, subaru_tcu_cvt_hitachi_m32r,
// subaru_tcu_cvt_mitsu_mh8104, subaru_tcu_cvt_mitsu_mh8111). Extracted
// because all five families independently carried a byte-for-byte identical
// copy of PhaseReporter/PhaseSequence.
namespace fastecu::flash
{

// Reports progress within one named phase of a PhaseSequence. The
// constructor emits `done == 0` immediately; update() clamps `done` to
// [last emitted, total - 1] so a caller cannot accidentally emit completion
// early, and only emits when the clamped value actually changes; complete()
// emits `done == total` unless that was already the last value emitted.
class PhaseReporter
{
  public:
    PhaseReporter(IEventSink& events, std::string_view name, int index, int count, int total)
        : events_(events), name_(name), index_(index), count_(count), total_(total)
    {
        emit(0);
    }

    void update(int done)
    {
        const int maximum_incomplete = std::max(0, total_ - 1);
        const int next = std::clamp(done, last_, maximum_incomplete);
        if (next != last_)
        {
            emit(next);
        }
    }

    void complete()
    {
        if (last_ != total_)
        {
            emit(total_);
        }
    }

  private:
    void emit(int done)
    {
        last_ = done;
        events_.phase_progress({name_, index_, count_, done, total_});
    }

    IEventSink& events_;
    std::string_view name_;
    int index_;
    int count_;
    int total_;
    int last_ = 0;
};

// Hands out PhaseReporters for a fixed-size sequence of named phases,
// numbering them 1..count in call order.
class PhaseSequence
{
  public:
    PhaseSequence(IEventSink& events, int count) : events_(events), count_(count)
    {
    }

    PhaseReporter start(std::string_view name, int total)
    {
        return PhaseReporter(events_, name, ++index_, count_, total);
    }

  private:
    IEventSink& events_;
    int count_;
    int index_ = 0;
};

} // namespace fastecu::flash
