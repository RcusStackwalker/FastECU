#include "src/platform/desktop/common/transport/desktop_logging_protocol_registration.h"

#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"
#include "src/backend/logging/protocols/portable_mut_dma_logging_protocol.h"
#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"
#include "src/platform/desktop/common/logging/cdbg_serial_setup.h"
#include "src/platform/desktop/common/logging/runtime/logging_engine.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/transport/fastecu_can_transport.h"
#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"

namespace fastecu::desktop::logging
{
void RegisterDesktopLoggingProtocols(LoggingEngine& engine, SerialPortActions& serial, fastecu::IClock& clock)
{
    engine.RegisterProtocol("MUT_DMA",
                            [&serial](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
                            {
                                auto transport = std::make_unique<mutdma::FastEcuKlineTransport>(&serial);
                                auto init = std::make_unique<mutdma::AlreadyInMode>(125000);
                                return std::make_unique<fastecu::logging::MutDmaLoggingProtocol>(
                                    std::move(transport), std::move(init), snapshot.session.Channels());
                            });

    engine.RegisterProtocol(
        "CDBG",
        [&serial](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
            -> fastecu::Result<std::unique_ptr<fastecu::logging::LoggingProtocol>>
        {
            const auto configured = fastecu::desktop::logging::ConfigureCdbgSerial({
                .disable_iso14230 = [&serial]() { return serial.SetIsIso14230Connection(false); },
                .disable_iso14230_header = [&serial]() { return serial.SetAddIso14230Header(false); },
                .enable_raw_can = [&serial]() { return serial.SetIsCanConnection(true); },
                .disable_iso15765 = [&serial]() { return serial.SetIsIso15765Connection(false); },
                .select_11_bit_ids = [&serial]() { return serial.SetIs29BitId(false); },
                .select_500k_baud = [&serial]() { return serial.SetCanSpeed("500000"); },
                .select_reply_id = [&serial]()
                { return serial.SetCanDestinationAddress(mitsu_colt_can_cdbg::kReplyCanId); },
            });
            if (!configured)
            {
                return std::unexpected(configured.error());
            }
            const QString opened_port = serial.OpenSerialPort();
            if (opened_port.isEmpty() || !serial.IsSerialPortOpen())
            {
                return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "unable to open CAN adapter for CDBG logging");
            }
            auto transport = std::make_unique<cdbg::FastEcuCanTransport>(&serial);
            return std::unique_ptr<fastecu::logging::LoggingProtocol>(
                std::make_unique<fastecu::logging::CdbgLoggingProtocol>(std::move(transport),
                                                                        snapshot.session.Channels()));
        });

    engine.RegisterProtocol("SSM",
                            [&serial, &clock](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
                            {
                                auto transport = std::make_unique<FastEcuSsmTransport>(&serial);
                                bool target_is_ecu = snapshot.target_is_ecu;
                                bool use_openport2_adapter = serial.GetUseOpenport2Adapter();
                                return std::make_unique<fastecu::logging::SsmLoggingProtocol>(
                                    clock, std::move(transport), snapshot.session.Channels(), snapshot.response_offsets,
                                    target_is_ecu, use_openport2_adapter);
                            });
}
} // namespace fastecu::desktop::logging
