#include "src/backend/diagnostics/dtc_session.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/diagnostics/nrc_parser.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using namespace fastecu::diagnostics;
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::IsEmpty;
using ::testing::Not;

namespace
{
bytes::Bytes b(std::initializer_list<int> values)
{
    bytes::Bytes out;
    for (int v : values)
    {
        out.push_back(static_cast<bytes::Byte>(v));
    }
    return out;
}

std::vector<std::string> lines(const RecordingEventSink& sink, LogLevel level)
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
std::vector<std::string> vehicle_info_calls(const std::string& read, const std::string& prefix = "")
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

    fastecu::Result<DtcReport> run(ObdProtocol protocol, DtcOperation operation)
    {
        return run_dtc_session(DtcRequest{protocol, operation}, link, clock, token, events);
    }
};
} // namespace

TEST(DtcSession, Iso9141DirectReadRunsTheFullSequence)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    // Page 0x00: one frame, then no frame.
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x41, 0x00, 0xBE, 0x1F, 0xB8, 0x10, 0xCC}));
    h.link.queue_no_frame();
    // 13 more vehicle-info requests, each answered by one "no frame".
    for (int i = 0; i < 13; ++i)
    {
        h.link.queue_no_frame();
    }
    // Stored, then pending.
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x43, 0x01, 0x33, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();

    const auto report = h.run(ObdProtocol::Iso9141, DtcOperation::Read);

    ASSERT_THAT(report, IsOk());
    std::vector<std::string> expected{
        "open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A",
        "p1 35",
        "five_baud 33",
        "p1 25",
        "set_header Iso9141",
    };
    auto info = vehicle_info_calls("read_obd 200");
    info.insert(info.begin() + 1, "read_obd 200"); // page 0x00: a frame, then no frame
    expected.insert(expected.end(), info.begin(), info.end());
    expected.insert(expected.end(), {"write 03", "read_obd 200", "read_obd 200", "write 07", "read_obd 200",
                                     "read_obd 200", "set_header None", "reset"});
    EXPECT_THAT(h.link.calls, ElementsAreArray(expected));
    EXPECT_EQ(h.clock.elapsed(), 4500ms);
    ASSERT_EQ(report->supported_pids.size(), 1U);
    EXPECT_THAT(report->supported_pids[0].bitmap, ElementsAre(0xBE, 0x1F, 0xB8, 0x10));
    EXPECT_THAT(report->stored, ElementsAre(0x0133));
    EXPECT_THAT(report->pending, IsEmpty());
    const auto info_lines = lines(h.events, LogLevel::Info);
    EXPECT_THAT(info_lines, Contains("Supported PIDs 0x1-0x20: be 1f b8 10 "));
    EXPECT_THAT(info_lines, Contains("Stored DTCs: 01 33 00 00 00 00 "));
    EXPECT_THAT(info_lines, Contains("DTC: " + dtc_description(0x0133)));
    EXPECT_THAT(info_lines, Contains("Pending DTCs: 00 00 00 00 00 00 "));
    EXPECT_THAT(info_lines, Contains("Diagnostic trouble codes succesfully read!"));
    EXPECT_TRUE(h.link.script_consumed());
}

TEST(DtcSession, MultiFrameVinIsConcatenatedAndLoggedBothWays)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    // The 7 PID-support pages, "status since cleared", and "VIN length"
    // requests: all no-frame, so the run reaches the VIN request (09 02).
    for (int i = 0; i < 9; ++i)
    {
        h.link.queue_no_frame();
    }
    // Three K-Line frames for the VIN, exercising all three
    // unframe_data_response size tiers: <7 (n/a here), <10 (drop 5) for the
    // first two frames, and >=10 (drop 6) for the third.
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x49, 0x02, 0x31, 0x32, 0xCC}));
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x49, 0x02, 0x33, 0x34, 0xCC}));
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x49, 0x02, 0x35, 0x36, 0x37, 0x38, 0x39, 0xCC}));
    h.link.queue_no_frame(); // ends the VIN frame-collection loop

    const auto report = h.run(ObdProtocol::Iso9141, DtcOperation::Read);

    // Everything after the VIN request (CAL ID/CVN items, then stored DTCs)
    // gets the fake's default no-frame answer, so the stored-DTC request
    // fails with "no stored DTC response" -- assert on the logs, which are
    // emitted before that failure, rather than on the (absent) report.
    EXPECT_THAT(report, IsErr(ErrorKind::BadResponse));
    const auto info_lines = lines(h.events, LogLevel::Info);
    EXPECT_THAT(info_lines, Contains("VIN: 31 32 33 34 36 37 38 39 "));
    EXPECT_THAT(info_lines, Contains("VIN: 12346789"));
}

TEST(DtcSession, OpenPortUsesPlainReadsAndTheAsciiFiveBaudCheck)
{
    Harness h;
    h.link.j2534 = true;
    h.link.queue_five_baud(b({0, 0, 0, 0, 0, '8', 0, '8'}));
    const auto report = h.run(ObdProtocol::Iso9141, DtcOperation::Read);
    // Stored DTC request gets no frame -> BadResponse after vehicle info.
    EXPECT_THAT(report, IsErr(ErrorKind::BadResponse));
    EXPECT_THAT(h.link.calls, Contains("set_header Iso9141"));
    EXPECT_THAT(h.link.calls, Contains("read 200"));
    EXPECT_THAT(h.link.calls, Not(Contains("read_obd 200")));
    EXPECT_THAT(h.link.calls, Not(Contains("p1 25"))); // only direct serial restores P1
}

TEST(DtcSession, Iso14230FastInitSuccess)
{
    Harness h;
    h.link.queue_read(b({0x83, 0xF1, 0x10, 0xC1, 0xE9, 0x8F, 0xAE}));
    static_cast<void>(h.run(ObdProtocol::Iso14230, DtcOperation::Read));
    ASSERT_GE(h.link.calls.size(), 4U);
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin(), h.link.calls.begin() + 4),
                ElementsAre("open kline header=Iso14230 iso14230=true baud=10400 start=C0 tester=F1 target=33",
                            "fast_init 81", "read_obd 200", "write 01 00"));
    EXPECT_THAT(lines(h.events, LogLevel::Info), Contains("iso14230 fast init mode succesfully completed."));
}

TEST(DtcSession, RejectedFastInitFallsBackToFiveBaud)
{
    Harness h;
    h.link.queue_read(b({0x83, 0xF1, 0x10, 0x00, 0x00, 0x00}));
    h.link.queue_five_baud(b({0x55, 0xEF, 0x8F}));
    static_cast<void>(h.run(ObdProtocol::Iso14230, DtcOperation::Read));
    EXPECT_THAT(lines(h.events, LogLevel::Error), Contains("iso14230 fast init mode failed."));
    ASSERT_GE(h.link.calls.size(), 8U);
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin() + 3, h.link.calls.begin() + 8),
                ElementsAre("open kline header=None iso14230=false baud=10400 start=C0 tester=F1 target=33", "p1 35",
                            "five_baud 33", "p1 25", "set_header Iso14230"));
}

TEST(DtcSession, FacadeFastInitFailureFallsBackSilently)
{
    Harness h;
    h.link.queue_fast_init(fastecu::fail(ErrorKind::Disconnected, "fast_init failed"));
    h.link.queue_five_baud(b({0x55, 0xEF, 0x8F}));
    static_cast<void>(h.run(ObdProtocol::Iso14230, DtcOperation::Read));
    EXPECT_THAT(lines(h.events, LogLevel::Error), Not(Contains("iso14230 fast init mode failed.")));
    EXPECT_THAT(h.link.calls, Contains("five_baud 33"));
    EXPECT_THAT(h.link.calls, Contains("set_header Iso14230"));
}

TEST(DtcSession, RejectedFiveBaudFailsAndStillRestoresTheLink)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x00, 0x00}));
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::BadResponse));
    EXPECT_THAT(lines(h.events, LogLevel::Error), Contains("iso9141 five baud init failed."));
    EXPECT_THAT(h.link.calls,
                ElementsAre("open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A", "p1 35",
                            "five_baud 33", "p1 25", "set_header None", "reset"));
}

TEST(DtcSession, ShortFiveBaudResponseFailsCleanly)
{
    Harness h;
    h.link.j2534 = true;
    h.link.queue_five_baud(b({0x00, 0x08}));
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::BadResponse));
    EXPECT_THAT(lines(h.events, LogLevel::Info), Contains("Init response: 00 08 "));
}

TEST(DtcSession, Iso15765ReadDecodesCanFrames)
{
    Harness h;
    h.link.j2534 = true;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00})); // init answer
    for (int i = 0; i < 14; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x43, 0x01, 0x01, 0x33}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x47, 0x00, 0x00, 0x00}));
    h.link.queue_no_frame();
    const auto report = h.run(ObdProtocol::Iso15765, DtcOperation::Read);
    ASSERT_THAT(report, IsOk());
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin(), h.link.calls.begin() + 4),
                ElementsAre("open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                            "write 00 00 07 E0 01 00", "read 2000", "write 00 00 07 E0 01 00"));
    EXPECT_THAT(report->stored, ElementsAre(0x0133));
}

TEST(DtcSession, Iso15765InitNrcIsLoggedFromOffsetThree)
{
    Harness h;
    h.link.j2534 = true;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x7F, 0x01, 0x12}));
    EXPECT_THAT(h.run(ObdProtocol::Iso15765, DtcOperation::Read), IsErr(ErrorKind::BadResponse));
    const bytes::Bytes nrc_frame = b({0xE8, 0x7F, 0x01, 0x12});
    EXPECT_THAT(lines(h.events, LogLevel::Error), Contains("Wrong response from ECU: " + nrc_description(nrc_frame)));
}

TEST(DtcSession, NrcOnStoredDtcsLogsAndFails)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    for (int i = 0; i < 14; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x7F, 0x03, 0x22, 0xCC}));
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::BadResponse));
    const bytes::Bytes nrc_frame = b({0x7F, 0x03, 0x22, 0xCC});
    EXPECT_THAT(lines(h.events, LogLevel::Error), Contains("Wrong response from ECU: " + nrc_description(nrc_frame)));
}

TEST(DtcSession, WrongPidIsLoggedAndDiscarded)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x41, 0x20, 0xBE, 0xCC}));
    static_cast<void>(h.run(ObdProtocol::Iso9141, DtcOperation::Read));
    EXPECT_THAT(lines(h.events, LogLevel::Error), Contains("Wrong response from ECU: 48 6b 10 41 20 be cc "));
    EXPECT_THAT(h.link.calls, Contains("write 01 20")); // the loop moved on
}

TEST(DtcSession, HighPidPagesAreAccepted)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    for (int i = 0; i < 4; ++i)
    {
        h.link.queue_no_frame(); // pages 00, 20, 40, 60
    }
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x41, 0x80, 0x01, 0x02, 0x03, 0x04, 0xCC}));
    h.link.queue_no_frame();
    static_cast<void>(h.run(ObdProtocol::Iso9141, DtcOperation::Read));
    EXPECT_THAT(lines(h.events, LogLevel::Info), Contains("Supported PIDs 0x81-0xa0: 01 02 03 04 "));
}

TEST(DtcSession, ClearSucceedsOnPositiveAcknowledgement)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    for (int i = 0; i < 14; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x44, 0xCC}));
    const auto report = h.run(ObdProtocol::Iso9141, DtcOperation::Clear);
    ASSERT_THAT(report, IsOk());
    EXPECT_TRUE(report->cleared);
    EXPECT_THAT(lines(h.events, LogLevel::Info), Contains("Diagnostic trouble codes succesfully cleared!"));
    EXPECT_THAT(h.link.calls, Contains("write 04"));
}

TEST(DtcSession, ClearWithoutAcknowledgementFails)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    for (int i = 0; i < 14; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Clear), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(h.link.calls.back(), "reset");
}

TEST(DtcSession, CancellationStopsAtTheFirstSleepAndRestoresTheLink)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    h.token.set_cancelled(true);
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::Cancelled));
    EXPECT_THAT(h.link.calls, Not(Contains("write 01 00")));
    EXPECT_THAT(std::vector<std::string>(h.link.calls.end() - 2, h.link.calls.end()),
                ElementsAre("set_header None", "reset"));
}

TEST(DtcSession, OpenFailureEndsRunAfterEpilogue)
{
    Harness h;
    h.link.queue_open(fastecu::fail(ErrorKind::Disconnected, "adapter did not open a port"));
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::Disconnected));
    EXPECT_THAT(h.link.calls,
                ElementsAre("open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A",
                            "set_header None", "reset"));
}
