#include "src/backend/protocol/testing/fake_diagnostic_link.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::diagnostics::CanLinkConfig;
using fastecu::diagnostics::FakeDiagnosticLink;
using fastecu::diagnostics::KlineHeader;
using fastecu::diagnostics::KlineLinkConfig;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAre;
using ::testing::Optional;
using namespace std::chrono_literals;

TEST(FakeDiagnosticLink, RecordsEveryCallInOrder)
{
    FakeDiagnosticLink link;
    FakeCancellationToken token;
    ASSERT_THAT(link.open(KlineLinkConfig{.header = KlineHeader::Iso14230,
                                          .iso14230_connection = true,
                                          .baud = 10400,
                                          .start_byte = 0xC0,
                                          .tester_id = 0xF1,
                                          .target_id = 0x33}),
                IsOk());
    ASSERT_THAT(
        link.open(CanLinkConfig{
            .iso15765 = true, .bitrate = 500000, .extended_id = false, .source_id = 0x7E0, .destination_id = 0x7E8}),
        IsOk());
    ASSERT_THAT(link.set_header(KlineHeader::Iso9141), IsOk());
    ASSERT_THAT(link.set_p1_max(35ms), IsOk());
    ASSERT_THAT(link.five_baud_init(0x33), IsOk());
    ASSERT_THAT(link.fast_init(bytes::Bytes{0x81}), IsOk());
    ASSERT_THAT(link.write(bytes::Bytes{0x01, 0x00}), IsOk());
    ASSERT_THAT(link.read(200ms, token), IsOk());
    ASSERT_THAT(link.read_obd(200ms, token), IsOk());
    ASSERT_THAT(link.reset(), IsOk());

    EXPECT_THAT(link.calls,
                ElementsAre("open kline header=Iso14230 iso14230=true baud=10400 start=C0 tester=F1 target=33",
                            "open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                            "set_header Iso9141", "p1 35", "five_baud 33", "fast_init 81", "write 01 00", "read 200",
                            "read_obd 200", "reset"));
}

TEST(FakeDiagnosticLink, ServesQueuedOutcomesThenNoFrame)
{
    FakeDiagnosticLink link;
    FakeCancellationToken token;
    link.queue_read(bytes::Bytes{0x41, 0x00});
    link.queue_read_error(ErrorKind::Disconnected);
    link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08});
    link.queue_fast_init(fastecu::fail(ErrorKind::Disconnected));
    link.queue_open(fastecu::fail(ErrorKind::Disconnected));

    EXPECT_THAT(link.read(200ms, token), IsOkAnd(Optional(ElementsAre(0x41, 0x00))));
    EXPECT_THAT(link.read_obd(200ms, token), IsErr(ErrorKind::Disconnected));
    EXPECT_THAT(link.read(200ms, token), IsOkAnd(std::nullopt));
    EXPECT_THAT(link.five_baud_init(0x33), IsOkAnd(ElementsAre(0x55, 0x08, 0x08)));
    EXPECT_THAT(link.five_baud_init(0x33), IsOkAnd(::testing::IsEmpty()));
    EXPECT_THAT(link.fast_init(bytes::Bytes{0x81}), IsErr(ErrorKind::Disconnected));
    EXPECT_THAT(link.open(KlineLinkConfig{}), IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(link.script_consumed());
}

TEST(FakeDiagnosticLink, ReadHonoursCancellation)
{
    FakeDiagnosticLink link;
    FakeCancellationToken token(true);
    link.queue_read(bytes::Bytes{0x41});
    EXPECT_THAT(link.read(200ms, token), IsErr(ErrorKind::Cancelled));
    EXPECT_FALSE(link.script_consumed());
}
