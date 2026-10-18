// Records the Mitsubishi Colt M32R CAN wire transcripts under
// src/backend/flash/ecu/testdata/mitsu_colt_m32r_can/ from the executor as it
// stands, driven by the same scripts its executor test uses:
//
//   bazel run //src/backend/flash/ecu/testing:record_mitsu_colt_m32r_can_transcripts -- \
//       --out "$PWD/src/backend/flash/ecu/testdata/mitsu_colt_m32r_can"
//
// The transcripts are regenerated from a known-good executor, never edited by
// hand.
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"
#include "src/backend/flash/ecu/testing/mitsu_colt_m32r_can_scripts.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/transcript_can_flash_transport.h"
#include "src/backend/flash/testing/wire_transcript.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{

using fastecu::flash::ScriptedCanFlashTransport;
namespace colt = fastecu::flash::colt_testing;

// "none" in both header lines is the result of an audit, not a default:
// - Silence: UdsClient::Request turns a no-frame read into kTimeout,
//   CanFlashUdsChannel::Receive only passes it up, and the executor never
//   reads the transport itself (every exchange is FatalRequest/FatalQuery).
// - Deferrals: cancellation is refused by GuardedCanFlashTransport Write/Read
//   and by UdsClient::Request; each returns kCancelled at once and nothing
//   defers.
// If the guard, the executor, UdsClient or CanFlashUdsChannel gains a deferral
// (a token read that does not stop) or a tolerated silence, redo this audit and update
// these lines before re-recording.
constexpr std::string_view kHeader =
    "## Recorded by //src/backend/flash/ecu/testing:record_mitsu_colt_m32r_can_transcripts; regenerate, never edit.\n"
    "## critical sections: none\n"
    "## expected silence: none\n";

struct Scenario
{
    std::string name;
    fastecu::Result<fastecu::flash::FlashPlan> plan;
    bytes::Bytes image;
    std::function<void(ScriptedCanFlashTransport&)> script;
};

std::vector<Scenario> Scenarios()
{
    std::vector<Scenario> scenarios;
    scenarios.push_back(Scenario{
        .name = "read_384k",
        .plan = colt::ReadPlan(colt::kProtocol384),
        .image = colt::AddressMarkedImage(0x60000),
        .script =
            [](ScriptedCanFlashTransport& transport)
        {
            colt::ScriptBootloadHandshake(transport);
            colt::ScriptAddressMarkedRead(transport, 0x60000);
        },
    });
    const bytes::Bytes rom384 = colt::WriteRom384();
    scenarios.push_back(Scenario{
        .name = "write_384k",
        .plan = colt::WritePlan(rom384, colt::kProtocol384),
        .image = rom384,
        .script = [rom384](ScriptedCanFlashTransport& transport) { colt::ScriptWrite384KiB(transport, rom384); },
    });
    const bytes::Bytes rom512 = colt::WriteRom();
    scenarios.push_back(Scenario{
        .name = "write_512k_bootstrap",
        .plan = colt::WritePlan(rom512),
        .image = rom512,
        .script = [rom512](ScriptedCanFlashTransport& transport)
        { colt::ScriptWriteWithTopRegionBootstrap(transport, rom512); },
    });
    return scenarios;
}

bool Record(const Scenario& scenario, const std::filesystem::path& out_dir)
{
    if (!scenario.plan)
    {
        std::fprintf(stderr, "%s: plan rejected: %s\n", scenario.name.c_str(), scenario.plan.error().detail.c_str());
        return false;
    }
    ScriptedCanFlashTransport transport(fastecu::flash::ScriptedTransportInitialState::kOpen);
    scenario.script(transport);
    fastecu::FakeClock clock;
    fastecu::flash::RecordingTranscriptCanFlashTransport recorder(transport, clock);
    fastecu::RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    fastecu::flash::MitsuColtM32rCanExecutor executor;

    const auto result = executor.Execute(*scenario.plan, recorder, recorder, cancellation, events);
    if (!result)
    {
        std::fprintf(stderr, "%s: executor failed: %s\n", scenario.name.c_str(), result.error().detail.c_str());
        return false;
    }
    if (!transport.ScriptConsumed())
    {
        std::fprintf(stderr, "%s: script not consumed\n", scenario.name.c_str());
        return false;
    }

    const std::filesystem::path path = out_dir / (scenario.name + ".transcript");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << kHeader << fastecu::flash::FormatWireTranscript(recorder.Transcript(), scenario.image);
    if (!file)
    {
        std::fprintf(stderr, "%s: cannot write %s\n", scenario.name.c_str(), path.string().c_str());
        return false;
    }
    std::printf("wrote %s\n", path.string().c_str());
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    const std::span<char *> args(argv, static_cast<std::size_t>(argc));
    if (args.size() != 3 || std::string_view(args[1]) != "--out")
    {
        std::fprintf(stderr, "usage: %s --out <dir>\n", args.empty() ? "recorder" : args[0]);
        return 2;
    }
    const std::filesystem::path out_dir(args[2]);
    std::filesystem::create_directories(out_dir);
    for (const Scenario& scenario : Scenarios())
    {
        if (!Record(scenario, out_dir))
        {
            return 1;
        }
    }
    return 0;
}
