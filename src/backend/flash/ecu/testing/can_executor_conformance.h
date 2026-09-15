#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash::testing
{

// The IFlashExecutor contract every CAN family must satisfy. A family
// differing on any of these would be a bug, not a protocol fact -- which is
// what separates these from the protocol-sequence bodies that stay local (see
// denso_iso15765_can_common.h for why those stay local).
template <class Traits> class CanExecutorConformance : public ::testing::Test
{
};

TYPED_TEST_SUITE_P(CanExecutorConformance);

// Cancels as soon as the "Read ROM" phase reports its first sliver of
// progress. Shared across families because which phase name means "the read
// loop" is not itself a per-family fact -- every executor names it
// identically (flash_phase_progress.h).
class CancelOnFirstReadProgressSink final : public RecordingEventSink
{
  public:
    explicit CancelOnFirstReadProgressSink(ManualCancellationToken& source) : source_(source)
    {
    }
    void phase_progress(const PhaseProgressEvent& event) override
    {
        RecordingEventSink::phase_progress(event);
        if (event.phase_name == "Read ROM" && event.done > 0)
        {
            source_.cancel();
        }
    }

  private:
    ManualCancellationToken& source_;
};

TYPED_TEST_P(CanExecutorConformance, TransportSetupReturnsThePlansWireParameters)
{
    typename TypeParam::Executor executor;

    const auto setup = executor.transport_setup(TypeParam::readPlan());

    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->bitrate, TypeParam::kWire.bitrate);
    EXPECT_EQ(setup->request_id, TypeParam::kWire.request_id);
    EXPECT_EQ(setup->response_id, TypeParam::kWire.response_id);
    EXPECT_EQ(setup->extended_id, TypeParam::kWire.extended_id);
}

// A plan built for another family must be rejected by check_family before
// configure()/open() or any write -- the scripted transport is left
// completely untouched. The foreign plan itself (a DensoSh705xEepromCan read)
// is family-agnostic scaffolding, not part of any family's protocol
// sequence, so it is built the same way for every instantiation.
//
// EXPECT_FALSE(transport.last_config_.has_value()) is promoted here from
// mitsu_colt_m32r_can/subaru_hitachi_m32r_can's copies, which had it, over the
// three tcu_cvt copies, which did not.
TYPED_TEST_P(CanExecutorConformance, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    ManualCancellationToken cancellation;
    typename TypeParam::Executor executor;

    FlashPlanFields fields;
    fields.operation = FlashOperation::Read;
    fields.family = FlashFamily::DensoSh705xEepromCan;
    fields.transport = TransportKind::CanIso15765;
    fields.target_id = "sub_ecu_denso_sh705x_eeprom_can";
    fields.mcu_name = "SH7058";
    fields.transfer_region = MemoryRegion{.start = 0x0, .length = 0x100};
    fields.kernel = KernelImage{.id = "k", .load_address = 0xffff6004, .bytes = {0x01, 0x02}};
    fields.family_plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::Mode2,
        .security = DensoSecurityVariant::Stock,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };
    auto foreign = validate_and_build(std::move(fields));
    ASSERT_THAT(foreign, fastecu::testing::IsOk());

    const auto result = executor.execute(*foreign, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::InvalidConfig,
                                                    ::testing::HasSubstr("does not match this executor")));
    EXPECT_THAT(events.logs, ::testing::IsEmpty());
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_FALSE(transport.last_config_.has_value());
}

TYPED_TEST_P(CanExecutorConformance, RefusesATestWritePlanRatherThanWritingForReal)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    ManualCancellationToken cancellation;
    typename TypeParam::Executor executor;

    const auto result =
        executor.execute(TypeParam::handBuiltPlan(FlashOperation::TestWrite), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::Unsupported));
    EXPECT_EQ(transport.writesConsumed(), 0U);
    EXPECT_FALSE(transport.last_config_.has_value());
    EXPECT_THAT(events.logs, ::testing::IsEmpty());
}

// scriptUpToFirstFatalRead (per-family hook) scripts connect plus every
// exchange up to and including the first request whose reply legacy treats as
// fatal if absent, leaving that final exchange's reply unqueued -- so a test
// can supply whatever failure mode (a transport error, a bare timeout, or an
// empty frame) belongs at that exact point.
TYPED_TEST_P(CanExecutorConformance, ReadPropagatesADisconnectedTransport)
{
    ScriptedCanFlashTransport transport;
    TypeParam::scriptUpToFirstFatalRead(transport);
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    FakeClock clock;
    RecordingEventSink events;
    ManualCancellationToken cancellation;
    typename TypeParam::Executor executor;

    const auto result = executor.execute(TypeParam::readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(transport.scriptConsumed());
}

TYPED_TEST_P(CanExecutorConformance, ReadStopsAtTheNextChunkWhenCancelledMidRead)
{
    ScriptedCanFlashTransport transport;
    TypeParam::scriptBenchConnect(transport);
    TypeParam::scriptReadSetup(transport);
    // Exactly one page is scripted; the executor is cancelled while it is
    // being served, so the sweep must stop at the top of the next page.
    TypeParam::scriptFlashDump(transport, TypeParam::kBlockStart, TypeParam::kPageSize, TypeParam::kPageSize, 0x00);

    FakeClock clock;
    ManualCancellationToken cancellation;
    CancelOnFirstReadProgressSink events{cancellation};
    typename TypeParam::Executor executor;

    const auto result = executor.execute(TypeParam::readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_TRUE(transport.scriptConsumed());
    const RecordedPhaseProgress *last = nullptr;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.phase_name == "Read ROM")
        {
            last = &event;
        }
    }
    ASSERT_NE(last, nullptr);
    EXPECT_LT(last->done, last->total);
}

// The full read plan's read()s, in order, should carry this family's own
// probe timeout exactly as many times as its wire timing calls for --
// replacing what were two bare literals (a `== 9` and a `== 3`) in two
// different families with one named constant per family.
TYPED_TEST_P(CanExecutorConformance, ConnectProbesReadWithThisFamilysProbeTimeout)
{
    ScriptedCanFlashTransport transport;
    TypeParam::scriptBenchConnect(transport);
    TypeParam::scriptReadSetup(transport);
    TypeParam::scriptFlashDump(transport, TypeParam::kBlockStart, TypeParam::kBlockLength, TypeParam::kPageSize, 0xA5);
    TypeParam::scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    ManualCancellationToken cancellation;
    typename TypeParam::Executor executor;

    ASSERT_THAT(executor.execute(TypeParam::readPlan(), transport, clock, cancellation, events),
                fastecu::testing::IsOk());
    // kProbeCount is counted empirically against a full bench-connect +
    // read-setup + full dump + stop-command run, not hand-derived from the
    // source: for most families it is dominated by one read per dump chunk
    // (kBlockLength / kPageSize), plus a handful of connect-time reads at
    // this same timeout. Each family's traits struct states only what is
    // actually specific to it -- e.g. when the dump sweep reads at a
    // different timeout than the connect-time probes, or when there is no
    // separate probe policy at all.
    EXPECT_EQ(std::ranges::count(transport.readTimeouts(), TypeParam::kProbeTimeout), TypeParam::kProbeCount);
}

TYPED_TEST_P(CanExecutorConformance, ReadReportsAnEmptyReplyAsTimeout)
{
    ScriptedCanFlashTransport transport;
    TypeParam::scriptUpToFirstFatalRead(transport);
    transport.queue_no_frame();

    FakeClock clock;
    RecordingEventSink events;
    ManualCancellationToken cancellation;
    typename TypeParam::Executor executor;

    EXPECT_THAT(executor.execute(TypeParam::readPlan(), transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}

TYPED_TEST_P(CanExecutorConformance, ReadTimeoutPropagates)
{
    ScriptedCanFlashTransport transport;
    TypeParam::scriptUpToFirstFatalRead(transport);
    transport.queue_error(ErrorKind::Timeout, "no reply");

    FakeClock clock;
    RecordingEventSink events;
    ManualCancellationToken cancellation;
    typename TypeParam::Executor executor;

    EXPECT_THAT(executor.execute(TypeParam::readPlan(), transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::Timeout));
    EXPECT_TRUE(transport.scriptConsumed());
}

REGISTER_TYPED_TEST_SUITE_P(CanExecutorConformance, TransportSetupReturnsThePlansWireParameters,
                            RejectsAPlanFromAnotherFamilyBeforeAnyIo, RefusesATestWritePlanRatherThanWritingForReal,
                            ReadPropagatesADisconnectedTransport, ReadStopsAtTheNextChunkWhenCancelledMidRead,
                            ConnectProbesReadWithThisFamilysProbeTimeout, ReadReportsAnEmptyReplyAsTimeout,
                            ReadTimeoutPropagates);

} // namespace fastecu::flash::testing
