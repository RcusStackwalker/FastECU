#pragma once

#include <chrono>

namespace uds
{

// Per-exchange timing. Vendor sequencing quirks live at the call site; only
// the timing knobs each exchange genuinely needs are parameters here.
struct ExchangePolicy
{
    // Quiet period between the write and the first read. Several families
    // need one; zero skips the sleep entirely.
    std::chrono::milliseconds pre_read_delay{0};

    std::chrono::milliseconds read_timeout{500};

    // Read timeout used once the ECU has reported responsePending. Separate
    // from read_timeout because "busy, wait" legitimately takes much longer
    // than a normal reply.
    std::chrono::milliseconds pending_timeout{3000};

    // Guard against an ECU that reports responsePending forever.
    int max_pending_repeats = 10;
};

} // namespace uds
