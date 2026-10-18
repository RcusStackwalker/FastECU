// Replays the recorded Mitsubishi Colt M32R CAN wire transcripts through the
// executor behind BindFlashAttempt. The transcripts under
// testdata/mitsu_colt_m32r_can/ were recorded from the executor by
// //src/backend/flash/ecu/testing:record_mitsu_colt_m32r_can_transcripts;
// a divergence here means the bytes on the wire changed.
//
// Also holds the pilot's conformance cases for ADR 0025: cancellation reaches
// the Colt executor through the guarded transport BindFlashAttempt wraps it in.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "rules_cc/cc/runfiles/runfiles.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"
#include "src/backend/flash/ecu/testing/mitsu_colt_m32r_can_scripts.h"
#include "src/backend/flash/ecu/testing/recording_can_flash_transport.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/transcript_can_flash_transport.h"
#include "src/backend/flash/testing/wire_transcript.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::Status;
using fastecu::flash::FlashExecutionResult;
using fastecu::flash::FlashPlan;
using fastecu::flash::ICanFlashTransport;
using fastecu::flash::Iso15765Config;
using fastecu::flash::MitsuColtM32rCanExecutor;
using fastecu::flash::ReplayCanFlashTransport;
using fastecu::flash::ScriptedCanFlashTransport;
using rules_cc::cc::runfiles::Runfiles;
namespace colt = fastecu::flash::colt_testing;

std::string ReadTranscript(std::string_view name)
{
    std::string error;
    const std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(BAZEL_CURRENT_REPOSITORY, &error));
    if (runfiles == nullptr)
    {
        ADD_FAILURE() << "runfiles unavailable: " << error;
        return {};
    }
    const std::string path = runfiles->Rlocation(
        std::format("_main/src/backend/flash/ecu/testdata/mitsu_colt_m32r_can/{}.transcript", name));
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        ADD_FAILURE() << "cannot open " << path;
        return {};
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Replayed
{
    fastecu::Result<FlashExecutionResult> result;
    bool consumed = false;
};

// Parses the transcript against `image`, binds the replay transport to a
// fresh executor, and runs it with the replay object as the clock.
Replayed ReplayThroughRunner(const FlashPlan& plan, std::string_view transcript_name, bytes::ByteView image)
{
    auto transcript = fastecu::flash::ParseWireTranscript(ReadTranscript(transcript_name), image);
    if (!transcript)
    {
        return {.result = std::unexpected(transcript.error())};
    }
    auto replay = std::make_unique<ReplayCanFlashTransport>(std::move(*transcript));
    ReplayCanFlashTransport& wire = *replay;
    auto attempt =
        fastecu::flash::BindFlashAttempt(plan, std::make_unique<MitsuColtM32rCanExecutor>(), std::move(replay));
    fastecu::ManualCancellationToken cancellation;
    fastecu::RecordingEventSink events;
    Replayed replayed{.result = attempt->Run(wire, cancellation, events)};
    replayed.consumed = wire.Consumed();
    return replayed;
}

TEST(MitsuColtM32rCanTranscript, ReadReplays384KiB)
{
    const auto plan = colt::ReadPlan(colt::kProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const bytes::Bytes image = colt::AddressMarkedImage(0x60000);

    const Replayed replayed = ReplayThroughRunner(*plan, "read_384k", image);

    ASSERT_THAT(replayed.result, fastecu::testing::IsOk());
    ASSERT_TRUE(replayed.result->read_bytes.has_value());
    EXPECT_THAT(*replayed.result->read_bytes, test_bytes::BytesEq(image));
    EXPECT_TRUE(replayed.consumed);
}

TEST(MitsuColtM32rCanTranscript, WriteReplays384KiB)
{
    const bytes::Bytes rom = colt::WriteRom384();
    const auto plan = colt::WritePlan(rom, colt::kProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const Replayed replayed = ReplayThroughRunner(*plan, "write_384k", rom);

    EXPECT_THAT(replayed.result, fastecu::testing::IsOk());
    EXPECT_TRUE(replayed.consumed);
}

TEST(MitsuColtM32rCanTranscript, WriteReplays512KiBBootstrap)
{
    const bytes::Bytes rom = colt::WriteRom();
    const auto plan = colt::WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const Replayed replayed = ReplayThroughRunner(*plan, "write_512k_bootstrap", rom);

    EXPECT_THAT(replayed.result, fastecu::testing::IsOk());
    EXPECT_TRUE(replayed.consumed);
}

// Forwards everything to a scripted transport but hands its Write and Read a
// token that is never cancelled, so the scripted transport's own token checks
// cannot stop a run: only a check above it can. Optionally cancels the run's
// token right after a chosen write reaches the scripted transport.
class CancellationBlindCanFlashTransport final : public ICanFlashTransport
{
  public:
    explicit CancellationBlindCanFlashTransport(ScriptedCanFlashTransport& inner) : inner_(inner)
    {
    }

    // Cancels `cancellation` once `writes` writes have been consumed.
    void CancelAfterWrites(fastecu::FakeCancellationToken& cancellation, std::size_t writes)
    {
        cancel_ = &cancellation;
        cancel_after_writes_ = writes;
    }

    Status ResetConnection() override
    {
        return inner_.ResetConnection();
    }
    Status Configure(const Iso15765Config& config) override
    {
        return inner_.Configure(config);
    }
    Status Open() override
    {
        return inner_.Open();
    }
    Status Close() override
    {
        return inner_.Close();
    }
    void RequestUnblock() noexcept override
    {
        inner_.RequestUnblock();
    }
    Status Write(bytes::ByteView data, const fastecu::ICancellationToken& /*cancellation*/) override
    {
        Status written = inner_.Write(data, never_cancelled_);
        if (cancel_ != nullptr && inner_.WritesConsumed() == cancel_after_writes_)
        {
            cancel_->SetCancelled(true);
        }
        return written;
    }
    fastecu::Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                                      const fastecu::ICancellationToken& /*cancellation*/) override
    {
        return inner_.Read(timeout, never_cancelled_);
    }

  private:
    ScriptedCanFlashTransport& inner_;
    fastecu::ManualCancellationToken never_cancelled_;
    fastecu::FakeCancellationToken *cancel_ = nullptr;
    std::size_t cancel_after_writes_ = 0;
};

// ADR 0025: cancelling as the ROM read begins stops the run before the first
// read request reaches the scripted transport, which cannot see the cancel.
// This pins that a cancel is honored behind BindFlashAttempt; it does not
// single out the guard, because UdsClient::Request still checks the token
// before every send during the pilot. The next case is the one only the guard
// can pass.
TEST(MitsuColtM32rCanTranscript, CancelGoesThroughTheGuardedTransport)
{
    const auto plan = colt::ReadPlan(colt::kProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedCanFlashTransport handshake_only;
    colt::ScriptBootloadHandshake(handshake_only);
    const std::size_t handshake_writes = handshake_only.Sections().size();

    ScriptedCanFlashTransport scripted;
    colt::ScriptBootloadHandshake(scripted);
    colt::ScriptAddressMarkedRead(scripted, 0x60000);
    auto attempt = fastecu::flash::BindFlashAttempt(*plan, std::make_unique<MitsuColtM32rCanExecutor>(),
                                                    std::make_unique<CancellationBlindCanFlashTransport>(scripted));
    fastecu::FakeCancellationToken cancellation;
    fastecu::flash::PhaseCancellingEventSink events(cancellation, "Read ROM", 0);
    fastecu::FakeClock clock;

    const auto result = attempt->Run(clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(scripted.WritesConsumed(), handshake_writes);
    EXPECT_EQ(scripted.close_call_count, 1);
}

// A cancel that lands after a request is written and before its reply is read
// can only be refused by the guarded transport: UdsClient::Request checks the
// token before Send and never between Send and Receive (no pre-read delay is
// configured for these exchanges, so no cancellable sleep sits there either).
// The first ROM read request reaches the bus; its reply must never be read.
TEST(MitsuColtM32rCanTranscript, ACancelBetweenARequestAndItsReplyIsRefusedByTheGuard)
{
    const auto plan = colt::ReadPlan(colt::kProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ScriptedCanFlashTransport handshake_only;
    colt::ScriptBootloadHandshake(handshake_only);
    const std::size_t handshake_writes = handshake_only.Sections().size();

    ScriptedCanFlashTransport scripted;
    colt::ScriptBootloadHandshake(scripted);
    colt::ScriptAddressMarkedRead(scripted, 0x60000);
    auto blind = std::make_unique<CancellationBlindCanFlashTransport>(scripted);
    fastecu::FakeCancellationToken cancellation;
    blind->CancelAfterWrites(cancellation, handshake_writes + 1);
    auto attempt =
        fastecu::flash::BindFlashAttempt(*plan, std::make_unique<MitsuColtM32rCanExecutor>(), std::move(blind));
    fastecu::RecordingEventSink events;
    fastecu::FakeClock clock;

    const auto result = attempt->Run(clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(scripted.WritesConsumed(), handshake_writes + 1);
    // One read per handshake exchange, none for the ROM read request.
    EXPECT_EQ(scripted.ReadTimeouts().size(), handshake_writes);
    EXPECT_EQ(scripted.close_call_count, 1);
}

} // namespace
