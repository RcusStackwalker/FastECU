#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_executor.h"

#include <array>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_sh705x_kline_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::composeBeWithChecksum;
using bytes::u24;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using namespace bytes::literals;
using namespace std::chrono_literals;

// ---- Wire transcription, independent of production helpers -------------

// Kernel frame: BE EF, u16 length (opcode + payload), opcode, payload, sum8.
bytes::Bytes beef(std::uint8_t opcode, bytes::ByteView payload = {})
{
    return composeBeWithChecksum(bytes::sum8, std::uint16_t{0xBEEF}, std::uint16_t(payload.size() + 1),
                                 bytes::Byte(opcode), payload);
}
// A positive kernel reply: BE EF, length, opcode|0x40, data, sum8.
bytes::Bytes beef_reply(std::uint8_t opcode, bytes::ByteView data = {})
{
    return beef(static_cast<std::uint8_t>(opcode | 0x40), data);
}
// SSM request tester 0xF0 -> target 0x10: 80 10 F0 len payload sum8.
bytes::Bytes ssm(bytes::ByteView payload)
{
    return composeBeWithChecksum(bytes::sum8, 0x80_b, 0x10_b, 0xF0_b, bytes::Byte(payload.size()), payload);
}
// SSM reply target -> tester: 80 F0 10 len payload sum8.
bytes::Bytes ssm_reply(bytes::ByteView payload)
{
    return composeBeWithChecksum(bytes::sum8, 0x80_b, 0xF0_b, 0x10_b, bytes::Byte(payload.size()), payload);
}

constexpr auto kKeyTable =
    std::to_array<std::uint16_t>({0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608, 0xAFB8, 0x503D,
                                  0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0});
constexpr auto kEncryptTable = std::to_array<std::uint16_t>({0x7856, 0xCE22, 0xF513, 0x6E86});

const bytes::Bytes kKernelIdRequest{0xBE, 0xEF, 0x00, 0x01, 0x01, 0xAF};
const bytes::Bytes kSeed{0x11, 0x22, 0x33, 0x44};
const bytes::Bytes kKernelBytes{0xAA, 0xBB, 0xCC, 0xDD};
// upload_kernel() transform of kKernelBytes, worked by hand in Task 5.
const bytes::Bytes kBalancedKernel{0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x8D, 0xC8};

bytes::Bytes kernel_id_reply()
{
    return beef_reply(0x01, bytes::Bytes{'S', 'S', 'M', 'K'});
}

KernelImage kernel_for(std::string_view mcu)
{
    return KernelImage{.id = "k", .load_address = mcu == "SH7055" ? 0xFFFF6004U : 0xFFFF3000U, .bytes = kKernelBytes};
}

FlashPlan make_plan(FlashOperation operation, std::string_view protocol = "sub_ecu_denso_sh7055_04",
                    std::string_view mcu = "SH7055", std::optional<bytes::Bytes> image = std::nullopt)
{
    if (operation != FlashOperation::Read && !image.has_value())
    {
        image = bytes::Bytes(find_flash_device(mcu)->romsize, 0xFF);
    }
    auto plan = build_subaru_denso_sh705x_kline_plan(operation, protocol, mcu, std::move(image), kernel_for(mcu));
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

struct Harness
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoSh705xKlineExecutor executor;

    Result<FlashExecutionResult> run(const FlashPlan& plan)
    {
        return executor.execute(plan, transport, clock, cancellation, events);
    }
};

TEST(SubaruDensoSh705xKlineExecutor, BalancedKernelMatchesHandWorkedVectors)
{
    // AA BB CC DD +00 00 -> pad to 8 -> drop 2 -> AA BB CC DD 00 00.
    // Words (bytes past the end read as zero): AABBCCDD, 00000000.
    // u16 sum = 0xCCDD; 0x5AA5 - 0xCCDD = 0x8DC8 (mod 2^16).
    EXPECT_EQ(denso_sh705x_kline_balanced_kernel(kKernelBytes), kBalancedKernel);
    // 01 02 03 +00 00 -> 01 02 03 00 00 00 00 00 -> drop 2 -> 6 bytes.
    // u16 sum = 0x0300; balance = 0x57A5.
    EXPECT_EQ(denso_sh705x_kline_balanced_kernel(bytes::Bytes{0x01, 0x02, 0x03}),
              (bytes::Bytes{0x01, 0x02, 0x03, 0x00, 0x00, 0x00, 0x57, 0xA5}));
}

TEST(SubaruDensoSh705xKlineExecutor, BalancedKernelWordSumIsAlways5AA5)
{
    for (std::size_t size = 1; size <= 9; ++size)
    {
        SCOPED_TRACE(size);
        bytes::Bytes kernel(size);
        for (std::size_t i = 0; i < size; ++i)
        {
            kernel[i] = static_cast<bytes::Byte>(0x31 * (i + 1));
        }
        const bytes::Bytes out = denso_sh705x_kline_balanced_kernel(kernel);
        ASSERT_EQ(out.size() % 4, 0U);
        std::uint16_t sum = 0;
        for (std::size_t i = 0; i < out.size(); i += 4)
        {
            sum = static_cast<std::uint16_t>(sum + bytes::readU32Be(out, i));
        }
        EXPECT_EQ(sum, 0x5AA5);
    }
}

TEST(SubaruDensoSh705xKlineExecutor, TransportSetupIsNonIso14230At4800)
{
    SubaruDensoSh705xKlineExecutor executor;
    const auto config = executor.transport_setup(make_plan(FlashOperation::Read));
    ASSERT_THAT(config, IsOk());
    EXPECT_EQ(config->baud, 4800);
    EXPECT_FALSE(config->iso14230);
    EXPECT_EQ(config->tester_id, 0xF0);
    EXPECT_EQ(config->target_id, 0x10);
    EXPECT_EQ(config->parity, KlineParity::None);
}

TEST(SubaruDensoSh705xKlineExecutor, BoundAttemptResetsBeforeConfigure)
{
    auto transport = std::make_unique<ScriptedKlineFlashTransport>();
    auto *observed = transport.get();
    observed->set_baud_result_ = fail(ErrorKind::Disconnected, "stop after lifecycle");
    auto attempt = bind_flash_attempt(make_plan(FlashOperation::Read),
                                      std::make_unique<SubaruDensoSh705xKlineExecutor>(), std::move(transport));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    // execute() has not yet started using the transport (Tasks 6-8), so it
    // validates the plan and returns Unsupported without calling setBaud.
    EXPECT_THAT(attempt->run(clock, cancellation, events), IsErr(ErrorKind::Unsupported));
    // execute():67 reset_connection() precedes every setter and open_serial_port().
    EXPECT_THAT(observed->lifecycle_calls_, ::testing::ElementsAre("reset_connection", "configure", "open", "close"));
}

TEST(SubaruDensoSh705xKlineExecutor, CancellationAroundResetStopsBeforeConfigure)
{
    for (const std::size_t check : {1U, 2U})
    {
        SCOPED_TRACE(check);
        ScriptedKlineFlashTransport transport;
        FakeClock clock;
        FakeCancellationToken cancellation;
        cancellation.cancel_on_check(check);
        SubaruDensoSh705xKlineExecutor executor;

        EXPECT_THAT(executor.before_transport_configure(transport, clock, cancellation), IsErr(ErrorKind::Cancelled));
        EXPECT_EQ(transport.reset_call_count_, check == 1 ? 0 : 1);
    }
}

TEST(SubaruDensoSh705xKlineExecutor, ResetFailurePropagates)
{
    ScriptedKlineFlashTransport transport;
    transport.reset_result_ = fail(ErrorKind::Disconnected, "no adapter");
    FakeClock clock;
    FakeCancellationToken cancellation;
    SubaruDensoSh705xKlineExecutor executor;

    EXPECT_THAT(executor.before_transport_configure(transport, clock, cancellation), IsErr(ErrorKind::Disconnected));
}

TEST(SubaruDensoSh705xKlineExecutor, RejectsAForeignPlanBeforeIo)
{
    Harness h;
    FlashPlanFields fields{.operation = FlashOperation::Read,
                           .family = FlashFamily::SubaruUnisiaJecs,
                           .transport = TransportKind::Kline,
                           .target_id = "sub_ecu_unisia_jecs_m3779x",
                           .mcu_name = "M3779x",
                           .transfer_region = {0, 0x10000},
                           .erase_regions = {},
                           .image = std::nullopt,
                           .kernel = std::nullopt,
                           .family_plan = SubaruUnisiaJecsPlan{.initial_baud = 1953, .even_parity = true},
                           .confirmations = {}};
    auto foreign = validate_and_build(std::move(fields));
    ASSERT_THAT(foreign, IsOk());

    EXPECT_THAT(h.executor.transport_setup(*foreign), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(h.run(*foreign), IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(h.transport.writesConsumed(), 0U);
}

} // namespace
} // namespace fastecu::flash
