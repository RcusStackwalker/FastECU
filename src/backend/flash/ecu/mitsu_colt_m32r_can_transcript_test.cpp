// Replays the recorded Mitsubishi Colt M32R CAN wire transcripts through the
// executor behind BindFlashAttempt. The transcripts under
// testdata/mitsu_colt_m32r_can/ were recorded from the executor by
// //src/backend/flash/ecu/testing:record_mitsu_colt_m32r_can_transcripts;
// a divergence here means the bytes on the wire changed.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <expected>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "rules_cc/cc/runfiles/runfiles.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"
#include "src/backend/flash/ecu/testing/mitsu_colt_m32r_can_scripts.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/transcript_can_flash_transport.h"
#include "src/backend/flash/testing/wire_transcript.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{

using fastecu::flash::FlashExecutionResult;
using fastecu::flash::FlashPlan;
using fastecu::flash::MitsuColtM32rCanExecutor;
using fastecu::flash::ReplayCanFlashTransport;
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

} // namespace
