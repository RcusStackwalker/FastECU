#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.h"

#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/eeprom/eeprom_read_plan.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash
{
namespace
{

class NeverCancelled : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

class ToggleCancellation final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return cancelled_;
    }

    void cancel()
    {
        cancelled_ = true;
    }

  private:
    bool cancelled_ = false;
};

class ShortWriteTransport final : public ScriptedKlineFlashTransport
{
  public:
    Result<std::size_t> write(bytes::ByteView data) override
    {
        return data.empty() ? 0u : data.size() - 1;
    }
};

class DrainCancellingTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit DrainCancellingTransport(ToggleCancellation& cancellation) : cancellation_(cancellation)
    {
    }

    Result<OptionalBytes> read(int timeout_ms, const ICancellationToken& cancellation) override
    {
        if (timeout_ms == 10)
        {
            cancellation_.cancel();
        }
        return ScriptedKlineFlashTransport::read(timeout_ms, cancellation);
    }

    Result<std::size_t> write(bytes::ByteView data) override
    {
        write_attempts_.emplace_back(data.begin(), data.end());
        return ScriptedKlineFlashTransport::write(data);
    }

    std::vector<bytes::Bytes> write_attempts_;

  private:
    ToggleCancellation& cancellation_;
};

Result<FlashPlan> stock_plan(FlashOperation operation = FlashOperation::Read)
{
    return build_subaru_denso_mc68hc16y5_02_plan(
        operation, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
        operation == FlashOperation::Read ? std::nullopt
                                          : std::optional<bytes::Bytes>(bytes::Bytes(0x28000, 0)),
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

Result<FlashPlan> ecutek_plan(FlashOperation operation = FlashOperation::Read)
{
    return build_subaru_denso_mc68hc16y5_02_plan(
        operation, "sub_ecu_denso_mc68hc16y5_02_ecutek", "MC68HC16Y5",
        operation == FlashOperation::Read ? std::nullopt
                                          : std::optional<bytes::Bytes>(bytes::Bytes(0x28000, 0)),
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

bytes::Bytes framed(std::uint8_t opcode, bytes::ByteView extra = {})
{
    bytes::Bytes out{0xBE, 0xEF};
    const std::uint16_t datalen_plus_one = static_cast<std::uint16_t>(extra.size() + 1);
    out.push_back(static_cast<bytes::Byte>((datalen_plus_one >> 8) & 0xFF));
    out.push_back(static_cast<bytes::Byte>(datalen_plus_one & 0xFF));
    out.push_back(opcode);
    out.insert(out.end(), extra.begin(), extra.end());
    out.push_back(fastecu::checksum::checksum8(out, false));
    return out;
}

void script_stock_connect_and_upload(ScriptedKlineFlashTransport& transport)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:69-70,
    // 111-146, 220-281, and 1139-1168.
    transport.queue_no_frame();
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x4D, 0x00, 0xB3});
    transport.expectWrite(bytes::Bytes{
        0x53,
        0x02,
        0x00,
        0x00,
        0x00,
        0x10,
        0x64,
        0x67,
        0x39,
        0x41,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x9A,
    });
    transport.queueRead(bytes::Bytes{});
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K', 'I', 'D'}));
}

void script_read_page(ScriptedKlineFlashTransport& transport, std::uint32_t address,
                      bytes::Byte fill, std::uint8_t response_opcode = 0x43)
{
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:323-455.
    transport.expectWrite(framed(0x03, bytes::Bytes{
                                           0x00,
                                           static_cast<bytes::Byte>((address >> 16) & 0xFF),
                                           static_cast<bytes::Byte>((address >> 8) & 0xFF),
                                           static_cast<bytes::Byte>(address & 0xFF),
                                           0x04,
                                           0x00,
                                       }));
    transport.queueRead(framed(response_opcode, bytes::Bytes(0x400, fill)));
}

class CancelAfterFirstPageClock final : public FakeClock
{
  public:
    explicit CancelAfterFirstPageClock(ToggleCancellation& cancellation) : cancellation_(cancellation)
    {
    }

    Status sleep(int ms, const ICancellationToken& cancellation) override
    {
        Status result = FakeClock::sleep(ms, cancellation);
        if (result.has_value() && ms == 1 && !cancelled_after_page_)
        {
            cancelled_after_page_ = true;
            cancellation_.cancel();
        }
        return result;
    }

  private:
    ToggleCancellation& cancellation_;
    bool cancelled_after_page_ = false;
};

config::ConfigPaths eeprom_paths()
{
    return {.kernel_files_directory = "kernels/", .protocols_file = "protocols.cfg"};
}

Result<FlashPlan> different_family_plan()
{
    constexpr char kConfig[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
  <protocols>
    <protocol name="sub_ecu_eeprom_denso_sh7055_kline" alias="SH7055 EEPROM K-Line">
      <ecu>Denso SH7055</ecu><mcu>SH7055</mcu><kernel>kernel.bin</kernel><kernel_addr>0xFFFF6004</kernel_addr>
    </protocol>
  </protocols>
  <car_models>
    <car_model><make>Subaru</make><model>Impreza</model><version>WRX</version><protocol>sub_ecu_eeprom_denso_sh7055_kline</protocol></car_model>
  </car_models>
</config>)";
    InMemoryFileRepository files;
    files.files["protocols.cfg"] = std::vector<std::uint8_t>(kConfig, kConfig + sizeof(kConfig) - 1);
    files.files["kernels/kernel.bin"] = {0x01, 0x02, 0x03, 0x04};
    return build_eeprom_read_plan(eeprom_paths(), "sub_ecu_eeprom_denso_sh7055_kline",
                                  EepromReadMode::Mode2, files);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WrongFamilyPlanFails)
{
    auto plan = different_family_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectsViaWrx02InitAndUploadsPaddedKernel)
{
    auto plan = stock_plan(FlashOperation::Write);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:69-70.
    transport.queue_no_frame();
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-146.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x4D, 0x00, 0xB3});

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:220-281.
    // The four input bytes are padded to 16 before encryption; magic patches
    // encrypted offsets 2..3, and each trailing zero encrypts to 0x65.
    const bytes::Bytes upload{
        0x53,
        0x02,
        0x00,
        0x00,
        0x00,
        0x10,
        0x64,
        0x67,
        0x39,
        0x41,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x65,
        0x9A,
    };
    transport.expectWrite(upload);
    transport.queueRead(bytes::Bytes{});

    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K', 'I', 'D'}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_,
              (std::vector<ScriptedKlineFlashTransport::ControlLineAction>{
                  ScriptedKlineFlashTransport::ControlLineAction::DisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::PulseLec2,
              }));
    EXPECT_EQ(transport.operation_trace_.front(),
              ScriptedKlineFlashTransport::Operation::DisableLecLines);
    EXPECT_EQ(transport.operation_trace_.at(1),
              ScriptedKlineFlashTransport::Operation::Read10);
    EXPECT_EQ(transport.lec_2_pulse_timeouts_, (std::vector<int>{200}));
    EXPECT_EQ(transport.read_timeouts_, (std::vector<int>{10, 200, 200, 2000}));
    EXPECT_EQ(transport.close_call_count_, 1);
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-119,
    // 289-293, and 1139-1162: 200 + 200 + 50 + 1500 + 200 ms.
    EXPECT_EQ(clock.now_, 2150u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectFallsBackToKernelAlivePoll)
{
    auto plan = stock_plan(FlashOperation::Write);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:69-70.
    transport.queueRead(bytes::Bytes{0xDE, 0xAD}); // stale bytes are discarded
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-179.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_,
              (std::vector<ScriptedKlineFlashTransport::ControlLineAction>{
                  ScriptedKlineFlashTransport::ControlLineAction::DisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::PulseLec2,
                  ScriptedKlineFlashTransport::ControlLineAction::DisableLecLines,
              }));
    EXPECT_EQ(transport.close_call_count_, 1);
    EXPECT_EQ(transport.read_timeouts_, (std::vector<int>{10, 200, 2000}));
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-119,
    // 149-155, and 1139-1162: 200 + 200 + 50 + 100 + 200 ms.
    EXPECT_EQ(clock.now_, 750u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, EcutekUsesItsDistinctBootloaderAndKernelWireValues)
{
    auto plan = ecutek_plan(FlashOperation::Write);
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:115-146.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x4C, 0x00, 0xB4});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:204-281.
    transport.expectWrite(bytes::Bytes{
        0x53,
        0x02,
        0x00,
        0x00,
        0x00,
        0x10,
        0x60,
        0x63,
        0x39,
        0x40,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0x61,
        0xD3,
    });
    transport.queueRead(bytes::Bytes{});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_EQ(transport.baud_calls_, (std::vector<int>{11700, 62500}));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectFailsWithNoValidResponseAtAll)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-179.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queue_no_frame();

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, CancellationBeforeConnectStopsImmediately)
{
    class AlreadyCancelled : public ICancellationToken
    {
      public:
        bool cancelled() const override
        {
            return true;
        }
    } cancellation;
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ShortKlineWriteFailsBeforeRead)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ShortWriteTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST(SubaruDensoMc68hc16y5_02Executor, InitialDrainTransportErrorStopsBeforeBootloaderTraffic)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_error(ErrorKind::Disconnected, "drain failed");
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, CancellationAtInitialDrainStopsBeforeBootloaderWrite)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ToggleCancellation cancellation;
    DrainCancellingTransport transport(cancellation);
    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.write_attempts_.empty());
    EXPECT_EQ(transport.read_timeouts_, (std::vector<int>{10}));
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, CloseOnlyFailureIsReturnedAfterSuccessfulPhases)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    transport.close_result_ = fail(ErrorKind::Internal, "close failed");
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-179.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queueRead(framed(0x41, bytes::Bytes{'K'}));

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, PhaseFailureWinsOverCloseFailureAndLogsCleanupFailure)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    transport.queue_no_frame(); // legacy operation.cpp:69-70 initial drain
    transport.close_result_ = fail(ErrorKind::Internal, "close failed");
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-179.
    transport.expectWrite(bytes::Bytes{0x4D, 0xFF, 0xB4});
    transport.queueRead(bytes::Bytes{0x00, 0x00, 0x00});
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:1139-1168.
    transport.expectWrite(framed(0x01));
    transport.queue_no_frame();

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(transport.close_call_count_, 1);
    ASSERT_FALSE(events.logs.empty());
    EXPECT_EQ(events.logs.back(),
              (std::pair<LogLevel, std::string>{
                  LogLevel::Warning, "close failed after MC68HC16Y5_02 phase error"}));
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadReturnsAssembledPageBytes)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);

    bytes::Bytes expected;
    for (std::uint32_t address = 0; address < 0x28000; address += 0x400)
    {
        const auto fill = static_cast<bytes::Byte>(address / 0x400);
        script_read_page(transport, address, fill);
        expected.insert(expected.end(), 0x400, fill);
    }

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->operation, FlashOperation::Read);
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expected);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadRejectsMalformedPageResponse)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    script_read_page(transport, 0x00000000, 0xA5, 0x44);

    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.close_call_count_, 1);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadCancelsBetweenPages)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
    script_stock_connect_and_upload(transport);
    script_read_page(transport, 0x00000000, 0xA5);

    ToggleCancellation cancellation;
    CancelAfterFirstPageClock clock(cancellation);
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.writesConsumed(), 4u);
    EXPECT_EQ(transport.close_call_count_, 1);
}

} // namespace
} // namespace fastecu::flash
