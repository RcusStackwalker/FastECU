#include "src/backend/diagnostics/dtc_session.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <ranges>
#include <string>
#include <tuple>
#include <vector>

#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/diagnostics/nrc_parser.h"
#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using namespace fastecu::diagnostics;
using test_bytes::B;
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Not;

namespace
{
std::vector<std::string> Lines(const RecordingEventSink& sink, LogLevel level)
{
    std::vector<std::string> out;
    for (const auto& [l, message] : sink.logs)
    {
        if (l == level)
        {
            out.push_back(message);
        }
    }
    return out;
}

// The 14 vehicle-info requests, each answered by one "no frame" read.
std::vector<std::string> VehicleInfoCalls(const std::string& read, const std::string& prefix = "")
{
    std::vector<std::string> calls;
    for (const char *pid : {"00", "20", "40", "60", "80", "A0", "C0"})
    {
        calls.push_back("write " + prefix + "01 " + pid);
        calls.push_back(read);
    }
    calls.push_back("write " + prefix + "01 01");
    calls.push_back(read);
    for (const char *pid : {"01", "02", "03", "04", "05", "06"})
    {
        calls.push_back("write " + prefix + "09 " + pid);
        calls.push_back(read);
    }
    return calls;
}

struct Harness
{
    FakeDiagnosticLink link;
    FakeClock clock;
    FakeCancellationToken token;
    RecordingEventSink events;

    fastecu::Result<DtcReport> Run(ObdProtocol protocol, DtcOperation operation)
    {
        return RunDtcSession(DtcRequest{protocol, operation}, link, clock, token, events);
    }
};

void QueueClearSetup(Harness& h)
{
    h.link.QueueFiveBaud(B({0x55, 0x08, 0x08}));
    for (int i = 0; i < 14; ++i)
    {
        h.link.QueueNoFrame();
    }
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.QueueNoFrame();
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.QueueNoFrame();
}

std::vector<std::string> ClearCalls(int acknowledgement_reads)
{
    std::vector<std::string> calls{
        "open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A",
        "p1 35",
        "five_baud 33",
        "p1 25",
        "set_header Iso9141",
    };
    const auto info = VehicleInfoCalls("read_obd 200");
    calls.insert(calls.end(), info.begin(), info.end());
    calls.insert(calls.end(),
                 {"write 03", "read_obd 200", "read_obd 200", "write 07", "read_obd 200", "read_obd 200", "write 04"});
    for (int i = 0; i < acknowledgement_reads; ++i)
    {
        calls.emplace_back("read_obd 200");
    }
    calls.insert(calls.end(), {"set_header None", "reset"});
    return calls;
}
} // namespace

TEST(DtcSession, Iso9141DirectReadRunsTheFullSequence)
{
    Harness h;
    h.link.QueueFiveBaud(B({0x55, 0x08, 0x08}));
    // Page 0x00: one frame, then no frame.
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x41, 0x00, 0xBE, 0x1F, 0xB8, 0x10, 0xCC}));
    h.link.QueueNoFrame();
    // 13 more vehicle-info requests, each answered by one "no frame".
    for (int i = 0; i < 13; ++i)
    {
        h.link.QueueNoFrame();
    }
    // Stored, then pending.
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x43, 0x01, 0x33, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.QueueNoFrame();
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.QueueNoFrame();

    const auto report = h.Run(ObdProtocol::kIso9141, DtcOperation::kRead);

    ASSERT_THAT(report, IsOk());
    std::vector<std::string> expected{
        "open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A",
        "p1 35",
        "five_baud 33",
        "p1 25",
        "set_header Iso9141",
    };
    auto info = VehicleInfoCalls("read_obd 200");
    info.insert(info.begin() + 1, "read_obd 200"); // page 0x00: a frame, then no frame
    expected.insert(expected.end(), info.begin(), info.end());
    expected.insert(expected.end(), {"write 03", "read_obd 200", "read_obd 200", "write 07", "read_obd 200",
                                     "read_obd 200", "set_header None", "reset"});
    EXPECT_THAT(h.link.calls, ElementsAreArray(expected));
    EXPECT_EQ(h.clock.Elapsed(), 4500ms);
    ASSERT_EQ(report->supported_pids.size(), 1U);
    EXPECT_THAT(report->supported_pids[0].bitmap, ElementsAre(0xBE, 0x1F, 0xB8, 0x10));
    EXPECT_THAT(report->stored, ElementsAre(0x0133));
    EXPECT_THAT(report->pending, IsEmpty());
    const auto info_lines = Lines(h.events, LogLevel::kInfo);
    EXPECT_THAT(info_lines, Contains("Supported PIDs 0x1-0x20: be 1f b8 10 "));
    EXPECT_THAT(info_lines, Contains("Stored DTCs: 01 33 00 00 00 00 "));
    EXPECT_THAT(info_lines, Contains("DTC: " + DtcDescription(0x0133)));
    EXPECT_THAT(info_lines, Contains("Pending DTCs: 00 00 00 00 00 00 "));
    EXPECT_THAT(info_lines, Contains("Diagnostic trouble codes succesfully read!"));
    EXPECT_TRUE(h.link.ScriptConsumed());
}

TEST(DtcSession, MultiFrameVinIsConcatenatedAndLoggedBothWays)
{
    Harness h;
    h.link.QueueFiveBaud(B({0x55, 0x08, 0x08}));
    // The 7 PID-support pages, "status since cleared", and "VIN length"
    // requests: all no-frame, so the run reaches the VIN request (09 02).
    for (int i = 0; i < 9; ++i)
    {
        h.link.QueueNoFrame();
    }
    // Three K-Line frames for the VIN, exercising all three
    // unframe_data_response size tiers: <7 (n/a here), <10 (drop 5) for the
    // first two frames, and >=10 (drop 6) for the third.
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x49, 0x02, 0x31, 0x32, 0xCC}));
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x49, 0x02, 0x33, 0x34, 0xCC}));
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x49, 0x02, 0x35, 0x36, 0x37, 0x38, 0x39, 0xCC}));
    h.link.QueueNoFrame(); // ends the VIN frame-collection loop

    const auto report = h.Run(ObdProtocol::kIso9141, DtcOperation::kRead);

    // Everything after the VIN request (CAL ID/CVN items, then stored DTCs)
    // gets the fake's default no-frame answer, so the stored-DTC request
    // fails with "no stored DTC response" -- assert on the logs, which are
    // emitted before that failure, rather than on the (absent) report.
    EXPECT_THAT(report, IsErr(ErrorKind::kBadResponse));
    const auto info_lines = Lines(h.events, LogLevel::kInfo);
    EXPECT_THAT(info_lines, Contains("VIN: 31 32 33 34 36 37 38 39 "));
    EXPECT_THAT(info_lines, Contains("VIN: 12346789"));
}

TEST(DtcSession, OpenPortUsesPlainReadsAndTheAsciiFiveBaudCheck)
{
    Harness h;
    h.link.j2534 = true;
    h.link.QueueFiveBaud(B({0, 0, 0, 0, 0, '8', 0, '8'}));
    const auto report = h.Run(ObdProtocol::kIso9141, DtcOperation::kRead);
    // Stored DTC request gets no frame -> BadResponse after vehicle info.
    EXPECT_THAT(report, IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(h.link.calls, Contains("set_header Iso9141"));
    EXPECT_THAT(h.link.calls, Contains("read 200"));
    EXPECT_THAT(h.link.calls, Not(Contains("read_obd 200")));
    EXPECT_THAT(h.link.calls, Not(Contains("p1 25"))); // only direct serial restores P1
}

TEST(DtcSession, Iso14230FastInitSuccess)
{
    Harness h;
    h.link.QueueRead(B({0x83, 0xF1, 0x10, 0xC1, 0xE9, 0x8F, 0xAE}));
    std::ignore = h.Run(ObdProtocol::kIso14230, DtcOperation::kRead);
    ASSERT_GE(h.link.calls.size(), 4U);
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin(), h.link.calls.begin() + 4),
                ElementsAre("open kline header=Iso14230 iso14230=true baud=10400 start=C0 tester=F1 target=33",
                            "fast_init 81", "read_obd 200", "write 01 00"));
    EXPECT_THAT(Lines(h.events, LogLevel::kInfo), Contains("iso14230 fast init mode succesfully completed."));
}

TEST(DtcSession, RejectedFastInitFallsBackToFiveBaud)
{
    Harness h;
    h.link.QueueRead(B({0x83, 0xF1, 0x10, 0x00, 0x00, 0x00}));
    h.link.QueueFiveBaud(B({0x55, 0xEF, 0x8F}));
    std::ignore = h.Run(ObdProtocol::kIso14230, DtcOperation::kRead);
    EXPECT_THAT(Lines(h.events, LogLevel::kError), Contains("iso14230 fast init mode failed."));
    ASSERT_GE(h.link.calls.size(), 8U);
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin() + 3, h.link.calls.begin() + 8),
                ElementsAre("open kline header=None iso14230=false baud=10400 start=C0 tester=F1 target=33", "p1 35",
                            "five_baud 33", "p1 25", "set_header Iso14230"));
}

TEST(DtcSession, FacadeFastInitFailureFallsBackSilently)
{
    Harness h;
    h.link.QueueFastInit(fastecu::Fail(ErrorKind::kDisconnected, "fast_init failed"));
    h.link.QueueFiveBaud(B({0x55, 0xEF, 0x8F}));
    std::ignore = h.Run(ObdProtocol::kIso14230, DtcOperation::kRead);
    EXPECT_THAT(Lines(h.events, LogLevel::kError), Not(Contains("iso14230 fast init mode failed.")));
    EXPECT_THAT(h.link.calls, Contains("five_baud 33"));
    EXPECT_THAT(h.link.calls, Contains("set_header Iso14230"));
}

TEST(DtcSession, RejectedFiveBaudFailsAndStillRestoresTheLink)
{
    Harness h;
    h.link.QueueFiveBaud(B({0x55, 0x00, 0x00}));
    EXPECT_THAT(h.Run(ObdProtocol::kIso9141, DtcOperation::kRead), IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(Lines(h.events, LogLevel::kError), Contains("iso9141 five baud init failed."));
    EXPECT_THAT(h.link.calls,
                ElementsAre("open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A", "p1 35",
                            "five_baud 33", "p1 25", "set_header None", "reset"));
}

TEST(DtcSession, ShortFiveBaudResponseFailsCleanly)
{
    Harness h;
    h.link.j2534 = true;
    h.link.QueueFiveBaud(B({0x00, 0x08}));
    EXPECT_THAT(h.Run(ObdProtocol::kIso9141, DtcOperation::kRead), IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(Lines(h.events, LogLevel::kInfo), Contains("Init response: 00 08 "));
}

TEST(DtcSession, Iso15765ReadDecodesCanFrames)
{
    Harness h;
    h.link.j2534 = true;
    h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00})); // init answer
    for (int i = 0; i < 14; ++i)
    {
        h.link.QueueNoFrame();
    }
    h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x43, 0x01, 0x01, 0x33}));
    h.link.QueueNoFrame();
    h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x47, 0x00, 0x00, 0x00}));
    h.link.QueueNoFrame();
    const auto report = h.Run(ObdProtocol::kIso15765, DtcOperation::kRead);
    ASSERT_THAT(report, IsOk());
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin(), h.link.calls.begin() + 4),
                ElementsAre("open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                            "write 00 00 07 E0 01 00", "read 2000", "write 00 00 07 E0 01 00"));
    EXPECT_THAT(report->stored, ElementsAre(0x0133));
}

TEST(DtcSession, Iso15765InitNrcDescribesActualPayload)
{
    Harness h;
    h.link.j2534 = true;
    h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x7F, 0x01, 0x12}));
    EXPECT_THAT(h.Run(ObdProtocol::kIso15765, DtcOperation::kRead), IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(Lines(h.events, LogLevel::kError), ElementsAre("Wrong response from ECU: Subfunction not supported"));
    EXPECT_THAT(h.link.calls,
                ElementsAre("open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                            "write 00 00 07 E0 01 00", "read 2000", "set_header None", "reset"));
    EXPECT_TRUE(h.link.ScriptConsumed());
}

TEST(DtcSession, NrcOnStoredDtcsLogsAndFails)
{
    Harness h;
    h.link.QueueFiveBaud(B({0x55, 0x08, 0x08}));
    for (int i = 0; i < 14; ++i)
    {
        h.link.QueueNoFrame();
    }
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x7F, 0x03, 0x22, 0xCC}));
    EXPECT_THAT(h.Run(ObdProtocol::kIso9141, DtcOperation::kRead), IsErr(ErrorKind::kBadResponse));
    const bytes::Bytes nrc_frame = B({0x7F, 0x03, 0x22, 0xCC});
    EXPECT_THAT(Lines(h.events, LogLevel::kError), Contains("Wrong response from ECU: " + NrcDescription(nrc_frame)));
}

TEST(DtcSession, WrongPidIsLoggedAndDiscarded)
{
    Harness h;
    h.link.QueueFiveBaud(B({0x55, 0x08, 0x08}));
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x41, 0x20, 0xBE, 0xCC}));
    std::ignore = h.Run(ObdProtocol::kIso9141, DtcOperation::kRead);
    EXPECT_THAT(Lines(h.events, LogLevel::kError), Contains("Wrong response from ECU: 48 6b 10 41 20 be cc "));
    EXPECT_THAT(h.link.calls, Contains("write 01 20")); // the loop moved on
}

TEST(DtcSession, HighPidPagesAreAccepted)
{
    Harness h;
    h.link.QueueFiveBaud(B({0x55, 0x08, 0x08}));
    for (int i = 0; i < 4; ++i)
    {
        h.link.QueueNoFrame(); // pages 00, 20, 40, 60
    }
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x41, 0x80, 0x01, 0x02, 0x03, 0x04, 0xCC}));
    h.link.QueueNoFrame();
    std::ignore = h.Run(ObdProtocol::kIso9141, DtcOperation::kRead);
    EXPECT_THAT(Lines(h.events, LogLevel::kInfo), Contains("Supported PIDs 0x81-0xa0: 01 02 03 04 "));
}

TEST(DtcSession, ClearSucceedsOnPositiveAcknowledgement)
{
    Harness h;
    QueueClearSetup(h);
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x44, 0xCC}));
    const auto report = h.Run(ObdProtocol::kIso9141, DtcOperation::kClear);
    ASSERT_THAT(report, IsOk());
    EXPECT_TRUE(report->cleared);
    EXPECT_THAT(Lines(h.events, LogLevel::kInfo), Contains("Diagnostic trouble codes succesfully cleared!"));
    EXPECT_THAT(h.link.calls, Contains("write 04"));
}

TEST(DtcSession, ClearWithoutAcknowledgementFails)
{
    Harness h;
    QueueClearSetup(h);
    EXPECT_THAT(h.Run(ObdProtocol::kIso9141, DtcOperation::kClear), IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(h.link.calls.back(), "reset");
}

TEST(DtcSession, CancellationStopsAtTheFirstSleepAndRestoresTheLink)
{
    Harness h;
    h.link.QueueFiveBaud(B({0x55, 0x08, 0x08}));
    h.token.SetCancelled(true);
    EXPECT_THAT(h.Run(ObdProtocol::kIso9141, DtcOperation::kRead), IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(h.link.calls, Not(Contains("write 01 00")));
    EXPECT_THAT(std::vector<std::string>(h.link.calls.end() - 2, h.link.calls.end()),
                ElementsAre("set_header None", "reset"));
}

TEST(DtcSession, OpenFailureEndsRunAfterEpilogue)
{
    Harness h;
    h.link.QueueOpen(fastecu::Fail(ErrorKind::kDisconnected, "adapter did not open a port"));
    EXPECT_THAT(h.Run(ObdProtocol::kIso9141, DtcOperation::kRead), IsErr(ErrorKind::kDisconnected));
    EXPECT_THAT(h.link.calls,
                ElementsAre("open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A",
                            "set_header None", "reset"));
}

TEST(DtcSession, ClearShortFrameThenAckSucceeds)
{
    Harness h;
    QueueClearSetup(h);
    h.link.QueueRead(B({0x48, 0x6B, 0x10}));
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x44, 0xCC}));

    const auto report = h.Run(ObdProtocol::kIso9141, DtcOperation::kClear);

    ASSERT_THAT(report, IsOk());
    EXPECT_TRUE(report->cleared);
    EXPECT_THAT(Lines(h.events, LogLevel::kError), IsEmpty());
    EXPECT_THAT(Lines(h.events, LogLevel::kInfo), Contains("Diagnostic trouble codes succesfully cleared!"));
    EXPECT_THAT(h.link.calls, ElementsAreArray(ClearCalls(2)));
    EXPECT_TRUE(h.link.ScriptConsumed());
}

TEST(DtcSession, ClearShortFrameThenSilenceFails)
{
    Harness h;
    QueueClearSetup(h);
    h.link.QueueRead(B({0x48, 0x6B, 0x10}));
    h.link.QueueNoFrame();

    EXPECT_THAT(h.Run(ObdProtocol::kIso9141, DtcOperation::kClear), IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(Lines(h.events, LogLevel::kError), IsEmpty());
    EXPECT_THAT(Lines(h.events, LogLevel::kInfo), Not(Contains("Diagnostic trouble codes succesfully cleared!")));
    EXPECT_THAT(h.link.calls, ElementsAreArray(ClearCalls(2)));
    EXPECT_TRUE(h.link.ScriptConsumed());
}

TEST(DtcSession, ClearNrcStopsReading)
{
    Harness h;
    QueueClearSetup(h);
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x7F, 0x04, 0x12, 0xCC}));

    EXPECT_THAT(h.Run(ObdProtocol::kIso9141, DtcOperation::kClear), IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(Lines(h.events, LogLevel::kError), ElementsAre("Wrong response from ECU: Subfunction not supported"));
    EXPECT_THAT(h.link.calls, ElementsAreArray(ClearCalls(1)));
    EXPECT_TRUE(h.link.ScriptConsumed());
}

TEST(DtcSession, ClearWrongServiceStopsReading)
{
    Harness h;
    QueueClearSetup(h);
    h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x43, 0xCC}));

    EXPECT_THAT(h.Run(ObdProtocol::kIso9141, DtcOperation::kClear), IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(Lines(h.events, LogLevel::kError), ElementsAre("Wrong response from ECU: 48 6b 10 43 cc "));
    EXPECT_THAT(h.link.calls, ElementsAreArray(ClearCalls(1)));
    EXPECT_TRUE(h.link.ScriptConsumed());
}

TEST(DtcSession, CanInitMissingOrShortReplyStopsBeforeVehicleInfo)
{
    // A missing frame differs from a present zero-length frame at the port.
    for (int length = -1; length <= 4; ++length)
    {
        SCOPED_TRACE(length);
        Harness h;
        h.link.j2534 = true;
        if (length < 0)
        {
            h.link.QueueNoFrame();
        }
        else
        {
            h.link.QueueRead(bytes::Bytes(static_cast<std::size_t>(length), 0x41));
        }

        EXPECT_THAT(h.Run(ObdProtocol::kIso15765, DtcOperation::kRead), IsErr(ErrorKind::kBadResponse));
        EXPECT_THAT(Lines(h.events, LogLevel::kError), IsEmpty());
        EXPECT_THAT(Lines(h.events, LogLevel::kInfo), Not(Contains("Requesting vehicle info, please wait...")));
        EXPECT_THAT(h.link.calls,
                    ElementsAre("open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                                "write 00 00 07 E0 01 00", "read 2000", "set_header None", "reset"));
        EXPECT_TRUE(h.link.ScriptConsumed());
    }
}

TEST(DtcSession, CanInitWrongServiceStopsBeforeVehicleInfo)
{
    for (int service : {0x00, 0x40, 0x42, 0xFF})
    {
        SCOPED_TRACE(service);
        Harness h;
        h.link.j2534 = true;
        const auto frame = B({0x00, 0x00, 0x07, 0xE8, service, 0x00});
        h.link.QueueRead(frame);

        EXPECT_THAT(h.Run(ObdProtocol::kIso15765, DtcOperation::kRead), IsErr(ErrorKind::kBadResponse));
        EXPECT_THAT(Lines(h.events, LogLevel::kError), ElementsAre("Wrong response from ECU: " + FormatHex(frame)));
        EXPECT_THAT(Lines(h.events, LogLevel::kInfo), Not(Contains("Requesting vehicle info, please wait...")));
        EXPECT_THAT(h.link.calls,
                    ElementsAre("open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                                "write 00 00 07 E0 01 00", "read 2000", "set_header None", "reset"));
        EXPECT_TRUE(h.link.ScriptConsumed());
    }
}

TEST(DtcSession, FastInitCancellationDoesNotFallBack)
{
    Harness h;
    h.link.QueueReadError(ErrorKind::kCancelled);

    EXPECT_THAT(h.Run(ObdProtocol::kIso14230, DtcOperation::kRead), IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(Lines(h.events, LogLevel::kError), IsEmpty());
    EXPECT_THAT(h.link.calls,
                ElementsAre("open kline header=Iso14230 iso14230=true baud=10400 start=C0 tester=F1 target=33",
                            "fast_init 81", "read_obd 200", "set_header None", "reset"));
    EXPECT_TRUE(h.link.ScriptConsumed());
}

namespace
{
bool IsFallibleCall(const std::string& call)
{
    return !call.starts_with("p1 ") && !call.starts_with("set_header ") && call != "reset";
}

// Fails each fallible link call of a successful session in turn: the injected
// error comes back unchanged, nothing runs after the failed call, and the
// epilogue still restores the link.
void ExpectEveryLinkFaultPassesThrough(ObdProtocol protocol, DtcOperation operation,
                                       const std::function<void(Harness&)>& script)
{
    Harness reference;
    script(reference);
    ASSERT_THAT(reference.Run(protocol, operation), IsOk());
    for (std::size_t call = 1; call <= reference.link.calls.size(); ++call)
    {
        SCOPED_TRACE(reference.link.calls.at(call - 1));
        if (!IsFallibleCall(reference.link.calls.at(call - 1)))
        {
            continue;
        }
        Harness h;
        script(h);
        h.link.fail_call = call;
        EXPECT_THAT(h.Run(protocol, operation), IsErrWith(ErrorKind::kDisconnected, Eq("injected fault")));
        std::vector<std::string> expected(reference.link.calls.begin(),
                                          reference.link.calls.begin() + static_cast<std::ptrdiff_t>(call));
        expected.insert(expected.end(), {"set_header None", "reset"});
        EXPECT_THAT(h.link.calls, ElementsAreArray(expected));
    }
}
} // namespace

TEST(DtcSession, EveryLinkFaultInAFiveBaudClearPassesThrough)
{
    ExpectEveryLinkFaultPassesThrough(ObdProtocol::kIso9141, DtcOperation::kClear,
                                      [](Harness& h)
                                      {
                                          QueueClearSetup(h);
                                          h.link.QueueRead(B({0x48, 0x6B, 0x10, 0x44, 0xCC}));
                                      });
}

TEST(DtcSession, EveryLinkFaultInACanReadPassesThrough)
{
    ExpectEveryLinkFaultPassesThrough(ObdProtocol::kIso15765, DtcOperation::kRead,
                                      [](Harness& h)
                                      {
                                          h.link.j2534 = true;
                                          h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00}));
                                          for (int i = 0; i < 14; ++i)
                                          {
                                              h.link.QueueNoFrame();
                                          }
                                          h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x43, 0x01, 0x01, 0x33}));
                                          h.link.QueueNoFrame();
                                          h.link.QueueRead(B({0x00, 0x00, 0x07, 0xE8, 0x47, 0x00, 0x00, 0x00}));
                                          h.link.QueueNoFrame();
                                      });
}

TEST(DtcSession, FastInitOpenFailureFallsBackToFiveBaud)
{
    Harness h;
    h.link.QueueOpen(fastecu::Fail(ErrorKind::kDisconnected, "adapter did not open a port"));
    h.link.QueueFiveBaud(B({0x55, 0x08, 0x08}));
    std::ignore = h.Run(ObdProtocol::kIso14230, DtcOperation::kRead);
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin(), h.link.calls.begin() + 3),
                ElementsAre("open kline header=Iso14230 iso14230=true baud=10400 start=C0 tester=F1 target=33",
                            "open kline header=None iso14230=false baud=10400 start=C0 tester=F1 target=33", "p1 35"));
}
