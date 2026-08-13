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

class ShortWriteTransport final : public ScriptedKlineFlashTransport
{
  public:
    Result<std::size_t> write(bytes::ByteView data) override
    {
        return data.empty() ? 0u : data.size() - 1;
    }
};

Result<FlashPlan> stock_plan(FlashOperation operation = FlashOperation::Read)
{
    return build_subaru_denso_mc68hc16y5_02_plan(
        operation, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5", std::nullopt,
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

Result<FlashPlan> ecutek_plan()
{
    return build_subaru_denso_mc68hc16y5_02_plan(
        FlashOperation::Read, "sub_ecu_denso_mc68hc16y5_02_ecutek", "MC68HC16Y5", std::nullopt,
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
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
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
    // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:111-119,
    // 289-293, and 1139-1162: 200 + 200 + 50 + 1500 + 200 ms.
    EXPECT_EQ(clock.now_, 2150u);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectFallsBackToKernelAlivePoll)
{
    auto plan = stock_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
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
}

TEST(SubaruDensoMc68hc16y5_02Executor, EcutekUsesItsDistinctBootloaderAndKernelWireValues)
{
    auto plan = ecutek_plan();
    ASSERT_TRUE(plan.has_value());
    ScriptedKlineFlashTransport transport;
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
    FakeClock clock;
    NeverCancelled cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.execute(*plan, transport, clock, cancellation, events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

} // namespace
} // namespace fastecu::flash
