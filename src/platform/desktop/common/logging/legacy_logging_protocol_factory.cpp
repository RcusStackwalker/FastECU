#include "src/platform/desktop/common/logging/legacy_logging_protocol_factory.h"

#include <exception>
#include <memory>
#include <utility>

#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"
#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"
#include "src/backend/logging/protocols/portable_mut_dma_logging_protocol.h"
#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"
#include "src/backend/protocol/imut_dma_init.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/transport/fastecu_can_transport.h"
#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"

namespace fastecu::desktop::logging
{

LegacyLoggingProtocolFactory::LegacyLoggingProtocolFactory(SerialPortActions& serial, QtClock& clock,
                                                           std::function<bool()> target_is_ecu)
    : dependencies_{
          .mut_dma_builder = [&serial](const std::vector<fastecu::logging::LoggingChannel>& channels) -> ProtocolResult
          {
              auto transport = std::make_unique<mutdma::FastEcuKlineTransport>(&serial);
              auto init = std::make_unique<mutdma::AlreadyInMode>(125000);
              return std::unique_ptr<fastecu::logging::LoggingProtocol>(
                  std::make_unique<fastecu::logging::MutDmaLoggingProtocol>(std::move(transport), std::move(init),
                                                                            channels));
          },
          .cdbg_builder = [&serial](const std::vector<fastecu::logging::LoggingChannel>& channels) -> ProtocolResult
          {
              auto transport = std::make_unique<cdbg::FastEcuCanTransport>(&serial);
              return std::unique_ptr<fastecu::logging::LoggingProtocol>(
                  std::make_unique<fastecu::logging::CdbgLoggingProtocol>(std::move(transport), channels));
          },
          .ssm_builder = [&serial, &clock](const std::vector<fastecu::logging::LoggingChannel>& channels,
                                           const std::vector<std::size_t>& response_offsets, bool is_ecu,
                                           bool use_openport2_adapter) -> ProtocolResult
          {
              auto transport = std::make_unique<FastEcuSsmTransport>(&serial);
              return std::unique_ptr<fastecu::logging::LoggingProtocol>(
                  std::make_unique<fastecu::logging::SsmLoggingProtocol>(
                      clock, std::move(transport), channels, response_offsets, is_ecu, use_openport2_adapter));
          },
          .cdbg_serial_setup =
              {
                  .disable_iso14230 = [&serial]() { return serial.set_is_iso14230_connection(false); },
                  .disable_iso14230_header = [&serial]() { return serial.set_add_iso14230_header(false); },
                  .enable_raw_can = [&serial]() { return serial.set_is_can_connection(true); },
                  .disable_iso15765 = [&serial]() { return serial.set_is_iso15765_connection(false); },
                  .select_11_bit_ids = [&serial]() { return serial.set_is_29_bit_id(false); },
                  .select_500k_baud = [&serial]() { return serial.set_can_speed("500000"); },
                  .select_reply_id = [&serial]()
                  { return serial.set_can_destination_address(MitsuColtCanCdbg::kReplyCanId); },
              },
          .open_cdbg_port = [&serial]() { return !serial.open_serial_port().isEmpty(); },
          .cdbg_port_is_open = [&serial]() { return serial.is_serial_port_open(); },
          .target_is_ecu = std::move(target_is_ecu),
          .use_openport2_adapter = [&serial]() { return serial.get_use_openport2_adapter(); },
      }
{
}

LegacyLoggingProtocolFactory::LegacyLoggingProtocolFactory(ConstructionDependencies dependencies, TestingTag)
    : dependencies_(std::move(dependencies))
{
}

LegacyLoggingProtocolFactory::ProtocolResult
LegacyLoggingProtocolFactory::create(const LegacyProtocolRequest& request) const
{
    const auto normalized = [](ProtocolResult result) -> ProtocolResult
    {
        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }
        if (!*result)
        {
            return fastecu::fail(fastecu::ErrorKind::Internal, "legacy protocol builder returned null");
        }
        return result;
    };

    try
    {
        switch (request.protocol)
        {
        case fastecu::logging::LoggingProtocolId::MutDma:
            return normalized(dependencies_.mut_dma_builder(request.channels));
        case fastecu::logging::LoggingProtocolId::Cdbg:
        {
            if (const auto configured = configure_cdbg_serial(dependencies_.cdbg_serial_setup); !configured.has_value())
            {
                return std::unexpected(configured.error());
            }
            if (!dependencies_.open_cdbg_port() || !dependencies_.cdbg_port_is_open())
            {
                return fastecu::fail(fastecu::ErrorKind::Disconnected, "unable to open CAN adapter for CDBG logging");
            }
            return normalized(dependencies_.cdbg_builder(request.channels));
        }
        case fastecu::logging::LoggingProtocolId::Ssm:
        {
            const bool target_is_ecu = dependencies_.target_is_ecu();
            const bool use_openport2_adapter = dependencies_.use_openport2_adapter();
            return normalized(dependencies_.ssm_builder(request.channels, request.ssm_response_offsets, target_is_ecu,
                                                        use_openport2_adapter));
        }
        }
        return fastecu::fail(fastecu::ErrorKind::Unsupported, "unsupported legacy logging protocol");
    }
    catch (const std::exception& error)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "legacy protocol builder threw an unknown exception");
    }
}

} // namespace fastecu::desktop::logging
