#include "src/platform/desktop/common/transport/desktop_logging_protocol_registration.h"

#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"
#include "src/backend/logging/protocols/portable_mut_dma_logging_protocol.h"
#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"
#include "src/platform/desktop/common/logging/cdbg_serial_setup.h"
#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/transport/fastecu_can_transport.h"
#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"

namespace fastecu::desktop::logging
{
void register_desktop_logging_protocols(LoggingEngine& engine, SerialPortActions& serial, fastecu::IClock& clock)
{
    engine.registerProtocol("MUT_DMA",
                            [&serial](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
                            {
                                auto transport = std::make_unique<mutdma::FastEcuKlineTransport>(&serial);
                                auto init = std::make_unique<mutdma::AlreadyInMode>(125000);
                                return std::make_unique<fastecu::logging::MutDmaLoggingProtocol>(
                                    std::move(transport), std::move(init), snapshot.session.channels());
                            });

    engine.registerProtocol(
        "CDBG",
        [&serial](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
            -> fastecu::Result<std::unique_ptr<fastecu::logging::LoggingProtocol>>
        {
            const auto configured = fastecu::desktop::logging::configure_cdbg_serial({
                .disable_iso14230 = [&serial]() { return serial.set_is_iso14230_connection(false); },
                .disable_iso14230_header = [&serial]() { return serial.set_add_iso14230_header(false); },
                .enable_raw_can = [&serial]() { return serial.set_is_can_connection(true); },
                .disable_iso15765 = [&serial]() { return serial.set_is_iso15765_connection(false); },
                .select_11_bit_ids = [&serial]() { return serial.set_is_29_bit_id(false); },
                .select_500k_baud = [&serial]() { return serial.set_can_speed("500000"); },
                .select_reply_id = [&serial]()
                { return serial.set_can_destination_address(MitsuColtCanCdbg::kReplyCanId); },
            });
            if (!configured)
            {
                return std::unexpected(configured.error());
            }
            const QString opened_port = serial.open_serial_port();
            if (opened_port.isEmpty() || !serial.is_serial_port_open())
            {
                return fastecu::fail(fastecu::ErrorKind::Disconnected, "unable to open CAN adapter for CDBG logging");
            }
            auto transport = std::make_unique<cdbg::FastEcuCanTransport>(&serial);
            return std::unique_ptr<fastecu::logging::LoggingProtocol>(
                std::make_unique<fastecu::logging::CdbgLoggingProtocol>(std::move(transport),
                                                                        snapshot.session.channels()));
        });

    engine.registerProtocol("SSM",
                            [&serial, &clock](const fastecu::desktop::logging::DesktopLoggingSnapshot& snapshot)
                            {
                                auto transport = std::make_unique<FastEcuSsmTransport>(&serial);
                                bool targetIsEcu = snapshot.target_is_ecu;
                                bool useOpenport2Adapter = serial.get_use_openport2_adapter();
                                return std::make_unique<fastecu::logging::SsmLoggingProtocol>(
                                    clock, std::move(transport), snapshot.session.channels(), snapshot.response_offsets,
                                    targetIsEcu, useOpenport2Adapter);
                            });
}
} // namespace fastecu::desktop::logging
