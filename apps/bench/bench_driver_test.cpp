#include "apps/bench/bench_driver.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string_view>

#include "apps/bench/bench_format.h"
#include "apps/bench/testing/fake_bench_environment.h"
#include "apps/bench/testing/fake_bench_files.h"
#include "apps/bench/testing/fake_bench_session.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"

namespace fastecu::bench
{
namespace
{

using testing::FakeBenchEnvironment;
using testing::FakeBenchFiles;
using testing::FakeBenchSession;

struct Harness
{
    FakeBenchSession session;
    FakeBenchEnvironment environment{session};
    FakeBenchFiles files;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream diagnostics;

    int run(std::vector<std::string_view> args, std::string script = {})
    {
        input.str(std::move(script));
        return run_cli(environment, files, args, input, output, diagnostics);
    }
};

std::size_t newlineCount(std::string_view text)
{
    return static_cast<std::size_t>(std::ranges::count(text, '\n'));
}

std::vector<Result<bytes::Bytes>> successfulUploadReplies()
{
    return {bytes::Bytes{0x74}, bytes::Bytes{0x76}, bytes::Bytes{0x74}, bytes::Bytes{0x76},
            bytes::Bytes{0x71, 0xE1, 0x00}};
}

bool sentErase(const FakeBenchSession& session)
{
    return std::ranges::find(session.requests, MitsuColtCan::buildRoutineErase()) != session.requests.end();
}

TEST(BenchDriver, JsonPortsIsOneObjectAndNeverRequestsASession)
{
    Harness harness;
    harness.environment.ports = {"op2-0", "op2-1"};

    const int code = harness.run({"--json", "ports"});

    EXPECT_EQ(code, 0);
    EXPECT_EQ(harness.environment.port_calls, 1);
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_EQ(newlineCount(harness.output.str()), 1u);
    EXPECT_TRUE(harness.output.str().starts_with("{"));
    EXPECT_NE(harness.output.str().find("\"step\":\"ports\""), std::string::npos);
    EXPECT_NE(harness.output.str().find("op2-0"), std::string::npos);
    EXPECT_TRUE(harness.diagnostics.str().empty());
}

TEST(BenchDriver, JsonPortsFailureIsAnObjectOnStdoutAndDiagnosticOnStderr)
{
    Harness harness;
    harness.environment.port_error = Error{ErrorKind::Disconnected, "adapter unavailable"};

    const int code = harness.run({"--json", "ports"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::Disconnected));
    EXPECT_EQ(newlineCount(harness.output.str()), 1u);
    EXPECT_NE(harness.output.str().find("\"ok\":false"), std::string::npos);
    EXPECT_NE(harness.output.str().find("adapter unavailable"), std::string::npos);
    EXPECT_NE(harness.diagnostics.str().find("adapter unavailable"), std::string::npos);
    EXPECT_EQ(harness.environment.session_calls, 0);
}

TEST(BenchDriver, JsonSetupFailurePreservesHandshakeTraffic)
{
    Harness harness;
    harness.environment.session_error = Error{ErrorKind::BadResponse, "wrong session echo"};
    harness.environment.setup_traffic = TrafficEvidence{.exchange_count = 1,
                                                        .tx = {0x10, 0x85},
                                                        .rx = {0x50, 0x81},
                                                        .last_tx = {0x10, 0x85},
                                                        .last_rx = {0x50, 0x81},
                                                        .elapsed_ms = 7};

    const int code = harness.run({"--json", "read", "0x200", "1"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::BadResponse));
    EXPECT_NE(harness.output.str().find("\"tx\":\"1085\""), std::string::npos);
    EXPECT_NE(harness.output.str().find("\"rx\":\"5081\""), std::string::npos);
    EXPECT_NE(harness.output.str().find("\"ms\":7"), std::string::npos);
    EXPECT_EQ(newlineCount(harness.output.str()), 1u);
}

TEST(BenchDriver, ExplicitConnectSuppressesTheImplicitHandshakeAndRunsExactlyOnce)
{
    Harness harness;
    harness.session.connect_traffic = TrafficEvidence{.exchange_count = 3,
                                                      .tx = {0x10, 0x85},
                                                      .rx = {0x50, 0x85},
                                                      .last_tx = {0x27, 0x06},
                                                      .last_rx = {0x67, 0x06},
                                                      .elapsed_ms = 9};

    const int code = harness.run({"connect"});

    EXPECT_EQ(code, 0);
    ASSERT_EQ(harness.environment.implicit_connect_requests.size(), 1u);
    EXPECT_FALSE(harness.environment.implicit_connect_requests.front());
    EXPECT_EQ(harness.session.connect_calls, 1);
}

TEST(BenchDriver, ARegularStepRequestsTheOneImplicitConnection)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x63, 0xAA}};

    const int code = harness.run({"read", "0x200", "1"});

    EXPECT_EQ(code, 0);
    ASSERT_EQ(harness.environment.implicit_connect_requests.size(), 1u);
    EXPECT_TRUE(harness.environment.implicit_connect_requests.front());
    EXPECT_EQ(harness.session.connect_calls, 0);
}

TEST(BenchDriver, InvalidFirstStepIsRejectedBeforeRequestingASession)
{
    Harness harness;

    const int code = harness.run({"read", "0x1000000", "1"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
    EXPECT_NE(harness.output.str().find("read 0x1000000 1"), std::string::npos);
}

TEST(BenchDriver, InvalidLaterStepIsRejectedBeforeAnyEarlierDestructiveStep)
{
    Harness harness;

    const int code = harness.run({"unlock", "--destructive", ":", "read", "nonsense", "1"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
    EXPECT_NE(harness.output.str().find("read nonsense 1"), std::string::npos);
    EXPECT_NE(harness.diagnostics.str().find("not a number"), std::string::npos);
}

TEST(BenchDriver, MissingLaterPayloadIsRejectedBeforeAnyEarlierDestructiveStep)
{
    Harness harness;

    const int code =
        harness.run({"unlock", "--destructive", ":", "download", "0x8000", "missing.bin", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
    EXPECT_NE(harness.output.str().find("download 0x8000 missing.bin"), std::string::npos);
    EXPECT_NE(harness.diagnostics.str().find("no such file"), std::string::npos);
}

TEST(BenchDriver, PreparedPayloadIsNotReloadedAfterAnEarlierDestructiveStep)
{
    Harness harness;
    harness.files.contents["payload.bin"] = {0xAA};
    harness.files.fail_on_repeated_load = true;
    harness.session.replies = {bytes::Bytes{0x7B, 0x00}, bytes::Bytes{0x74}, bytes::Bytes{0x76},
                               bytes::Bytes{0x74},       bytes::Bytes{0x76}, bytes::Bytes{0x71, 0xE1, 0x00}};

    const int code =
        harness.run({"unlock", "--destructive", ":", "download", "0x8000", "payload.bin", "--destructive"});

    EXPECT_EQ(code, 0);
    EXPECT_EQ(harness.files.load_calls.at("payload.bin"), 1);
    ASSERT_EQ(harness.session.requests.size(), 6u);
    EXPECT_EQ(harness.session.requests.front(), MitsuColtCan::buildRequestReflashUnlock());
    EXPECT_EQ(harness.session.requests[2], (bytes::Bytes{0x36, 0xAA}));
}

TEST(BenchDriver, InvalidLaterScriptLinePreventsEarlierDestructiveLine)
{
    Harness harness;

    const int code = harness.run({"--script", "-"}, "unlock --destructive\nread nonsense 1\n");

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
    EXPECT_NE(harness.output.str().find("script line 2: read nonsense 1"), std::string::npos);
}

TEST(BenchDriver, ScriptLineGlobalOptionsAreRejectedInsteadOfDiscarded)
{
    Harness harness;

    const int code = harness.run({"--json", "--script", "-"}, "read 0x200 1 --timeout 20\n");

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_EQ(newlineCount(harness.output.str()), 1u);
    EXPECT_NE(harness.output.str().find("script-line global option"), std::string::npos);
    EXPECT_NE(harness.diagnostics.str().find("script-line global option"), std::string::npos);
}

TEST(BenchDriver, JsonConnectFailureIsAnOutcomeWithTraffic)
{
    Harness harness;
    harness.session.connect_result = fail(ErrorKind::BadResponse, "bad key echo");
    harness.session.connect_traffic = TrafficEvidence{.exchange_count = 3,
                                                      .tx = {0x10, 0x85},
                                                      .rx = {0x50, 0x85},
                                                      .last_tx = {0x27, 0x06},
                                                      .last_rx = {0x67, 0x07},
                                                      .elapsed_ms = 12};

    const int code = harness.run({"--json", "connect"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::BadResponse));
    EXPECT_EQ(newlineCount(harness.output.str()), 1u);
    EXPECT_NE(harness.output.str().find("\"last_rx\":\"6707\""), std::string::npos);
    EXPECT_NE(harness.diagnostics.str().find("bad key echo"), std::string::npos);
}

TEST(BenchDriver, StandaloneEraseIsRejectedBeforeRequestingASession)
{
    Harness harness;

    const int code = harness.run({"erase", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_FALSE(sentErase(harness.session));
    EXPECT_NE(harness.diagnostics.str().find("erase helper"), std::string::npos);
    EXPECT_NE(harness.diagnostics.str().find("unlock"), std::string::npos);
}

TEST(BenchDriver, EraseRequiresANamedEraseHelperRatherThanAnyRoutineUpload)
{
    Harness harness;

    const int code = harness.run({"upload-routine", "write-page", "--destructive", ":", "unlock", "--destructive", ":",
                                  "erase", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
}

TEST(BenchDriver, UnlockBeforeEraseHelperDoesNotQualifyForErase)
{
    Harness harness;

    const int code = harness.run({"unlock", "--destructive", ":", "upload-routine", "erase-page", "--destructive", ":",
                                  "erase", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
}

TEST(BenchDriver, ScriptUnlockBeforeEraseHelperDoesNotQualifyForErase)
{
    Harness harness;

    const int code = harness.run({"--script", "-"}, "unlock --destructive\n"
                                                    "upload-routine erase-page --destructive\n"
                                                    "erase --destructive\n");

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
}

TEST(BenchDriver, EraseHelperFromOverrideDoesNotQualifyForErase)
{
    Harness harness;
    harness.files.contents["custom.bin"] = {0xAA};

    const int code = harness.run({"upload-routine", "erase-redirect", "--from", "custom.bin", "--destructive", ":",
                                  "unlock", "--destructive", ":", "erase", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
}

TEST(BenchDriver, DownloadBetweenEraseHelperAndUnlockInvalidatesThePlan)
{
    for (const std::vector<std::string_view>& args :
         {std::vector<std::string_view>{"upload-routine", "erase-page", "--destructive", ":", "download", "0x805568",
                                        "payload.bin", "--destructive", ":", "unlock", "--destructive", ":", "erase",
                                        "--destructive"},
          std::vector<std::string_view>{"--keep-going", "upload-routine", "erase-page", "--destructive", ":",
                                        "download", "0x900000", "payload.bin", "--destructive", ":", "unlock",
                                        "--destructive", ":", "erase", "--destructive"}})
    {
        Harness harness;
        harness.files.contents["payload.bin"] = {0xAA};

        const int code = harness.run(args);

        EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
        EXPECT_EQ(harness.environment.session_calls, 0);
        EXPECT_TRUE(harness.session.requests.empty());
    }
}

TEST(BenchDriver, ScriptDownloadBetweenEraseHelperAndUnlockInvalidatesThePlan)
{
    Harness harness;
    harness.files.contents["payload.bin"] = {0xAA};

    const int code = harness.run({"--keep-going", "--script", "-"}, "upload-routine erase-page --destructive\n"
                                                                    "download 0x805568 payload.bin --destructive\n"
                                                                    "unlock --destructive\n"
                                                                    "erase --destructive\n");

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
}

TEST(BenchDriver, DestructiveRawSendBetweenEraseHelperAndUnlockInvalidatesThePlan)
{
    Harness harness;

    const int code = harness.run({"upload-routine", "erase-page", "--destructive", ":", "send-raw", "22",
                                  "--destructive", ":", "unlock", "--destructive", ":", "erase", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 0);
    EXPECT_TRUE(harness.session.requests.empty());
}

TEST(BenchDriver, NamedEraseHelperAndUnlockPermitEraseInTheSameSession)
{
    Harness harness;
    harness.session.replies = successfulUploadReplies();
    harness.session.replies.push_back(bytes::Bytes{0x7B, 0x00});
    harness.session.replies.push_back(bytes::Bytes{0x71, 0xE0, 0x00});

    const int code = harness.run({"upload-routine", "erase-page", "--destructive", ":", "unlock", "--destructive", ":",
                                  "erase", "--destructive"});

    EXPECT_EQ(code, 0);
    EXPECT_EQ(harness.environment.session_calls, 1);
    EXPECT_TRUE(sentErase(harness.session));
}

TEST(BenchDriver, FailedEraseHelperUploadPreventsEraseUnderKeepGoing)
{
    Harness harness;
    harness.session.replies = {fail(ErrorKind::BadResponse, "helper upload failed"), bytes::Bytes{0x7B, 0x00}};

    const int code = harness.run({"--keep-going", "upload-routine", "erase-redirect", "--destructive", ":", "unlock",
                                  "--destructive", ":", "erase", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::BadResponse));
    EXPECT_EQ(harness.environment.session_calls, 1);
    EXPECT_FALSE(sentErase(harness.session));
    EXPECT_NE(harness.diagnostics.str().find("successful erase helper"), std::string::npos);
}

TEST(BenchDriver, FailedUnlockPreventsEraseUnderKeepGoing)
{
    Harness harness;
    harness.session.replies = successfulUploadReplies();
    harness.session.replies.push_back(fail(ErrorKind::BadResponse, "unlock failed"));

    const int code = harness.run({"--keep-going", "upload-routine", "erase-page", "--destructive", ":", "unlock",
                                  "--destructive", ":", "erase", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::BadResponse));
    EXPECT_EQ(harness.environment.session_calls, 1);
    EXPECT_FALSE(sentErase(harness.session));
    EXPECT_NE(harness.diagnostics.str().find("successful unlock"), std::string::npos);
}

TEST(BenchDriver, FailedReadInvalidatesEraseEligibilityUnderKeepGoing)
{
    Harness harness;
    harness.session.replies = successfulUploadReplies();
    harness.session.replies.push_back(fail(ErrorKind::BadResponse, "read failed"));
    harness.session.replies.push_back(bytes::Bytes{0x7B, 0x00});

    const int code = harness.run({"--keep-going", "upload-routine", "erase-page", "--destructive", ":", "read", "0x200",
                                  "1", ":", "unlock", "--destructive", ":", "erase", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::BadResponse));
    EXPECT_EQ(harness.environment.session_calls, 1);
    EXPECT_FALSE(sentErase(harness.session));
    EXPECT_NE(harness.diagnostics.str().find("no intervening destructive or failed step"), std::string::npos);
}

TEST(BenchDriver, ReadOnlyStepsPreserveTheExactEraseSequence)
{
    Harness harness;
    harness.session.replies = successfulUploadReplies();
    harness.session.replies.push_back(bytes::Bytes{0x63, 0xAA});
    harness.session.replies.push_back(bytes::Bytes{0x7B, 0x00});
    harness.session.replies.push_back(bytes::Bytes{0x63, 0xBB});
    harness.session.replies.push_back(bytes::Bytes{0x71, 0xE0, 0x00});

    const int code = harness.run({"upload-routine", "erase-redirect", "--destructive", ":", "read", "0x200", "1", ":",
                                  "unlock", "--destructive", ":", "read", "0x201", "1", ":", "erase", "--destructive"});

    EXPECT_EQ(code, 0);
    EXPECT_EQ(harness.environment.session_calls, 1);
    EXPECT_TRUE(sentErase(harness.session));
}

TEST(BenchDriver, ScriptLinesShareOneSessionAndErasePrerequisiteState)
{
    Harness harness;
    harness.session.replies = successfulUploadReplies();
    harness.session.replies.push_back(bytes::Bytes{0x7B, 0x00});
    harness.session.replies.push_back(bytes::Bytes{0x71, 0xE0, 0x00});

    const int code = harness.run({"--script", "-"}, "upload-routine erase-redirect --destructive\n"
                                                    "unlock --destructive\n"
                                                    "erase --destructive\n");

    EXPECT_EQ(code, 0);
    EXPECT_EQ(harness.environment.session_calls, 1);
    ASSERT_EQ(harness.environment.implicit_connect_requests.size(), 1u);
    EXPECT_TRUE(harness.environment.implicit_connect_requests.front());
    EXPECT_TRUE(sentErase(harness.session));
}

TEST(BenchDriver, ErasePrerequisitesDoNotCarryAcrossCliInvocations)
{
    Harness harness;
    harness.session.replies = successfulUploadReplies();
    harness.session.replies.push_back(bytes::Bytes{0x7B, 0x00});

    ASSERT_EQ(harness.run({"upload-routine", "erase-page", "--destructive", ":", "unlock", "--destructive"}), 0);
    const int code = harness.run({"erase", "--destructive"});

    EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
    EXPECT_EQ(harness.environment.session_calls, 1);
    EXPECT_FALSE(sentErase(harness.session));
}

TEST(BenchDriver, ExplicitConnectMustBeTheFirstSessionStepAndAppearOnlyOnce)
{
    for (const std::vector<std::string_view>& args :
         {std::vector<std::string_view>{"read", "0x200", "1", ":", "connect"},
          std::vector<std::string_view>{"connect", ":", "connect"}})
    {
        Harness harness;

        const int code = harness.run(args);

        EXPECT_EQ(code, exit_code_for(ErrorKind::InvalidConfig));
        EXPECT_EQ(harness.environment.session_calls, 0);
        EXPECT_EQ(harness.session.connect_calls, 0);
        EXPECT_TRUE(harness.session.requests.empty());
    }
}

TEST(BenchDriver, ExplicitConnectCanLeadAChainedSessionWithoutAnImplicitHandshake)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x63, 0xAA}};

    const int code = harness.run({"connect", ":", "read", "0x200", "1"});

    EXPECT_EQ(code, 0);
    EXPECT_EQ(harness.environment.session_calls, 1);
    ASSERT_EQ(harness.environment.implicit_connect_requests.size(), 1u);
    EXPECT_FALSE(harness.environment.implicit_connect_requests.front());
    EXPECT_EQ(harness.session.connect_calls, 1);
}

} // namespace
} // namespace fastecu::bench
