#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "src/backend/logging/logging_protocol.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/logging/cdbg_serial_setup.h"

class QtClock;
class SerialPortActions;

namespace fastecu::desktop::logging
{

struct LegacyProtocolRequest
{
    fastecu::logging::LoggingProtocolId protocol;
    std::vector<fastecu::logging::LoggingChannel> channels;
    std::vector<std::size_t> ssm_response_offsets;
};

class LegacyLoggingProtocolFactoryTestAccess;

class LegacyLoggingProtocolFactory
{
  public:
    LegacyLoggingProtocolFactory(SerialPortActions& serial, QtClock& clock, std::function<bool()> target_is_ecu);
    fastecu::Result<std::unique_ptr<fastecu::logging::LoggingProtocol>>
    create(const LegacyProtocolRequest& request) const;

  private:
    using ProtocolResult = fastecu::Result<std::unique_ptr<fastecu::logging::LoggingProtocol>>;
    using ProtocolBuilder =
        std::function<ProtocolResult(const std::vector<fastecu::logging::LoggingChannel>& channels)>;
    using SsmProtocolBuilder = std::function<ProtocolResult(
        const std::vector<fastecu::logging::LoggingChannel>& channels, const std::vector<std::size_t>& response_offsets,
        bool target_is_ecu, bool use_openport2_adapter)>;

    struct ConstructionDependencies
    {
        ProtocolBuilder mut_dma_builder;
        ProtocolBuilder cdbg_builder;
        SsmProtocolBuilder ssm_builder;
        CdbgSerialSetupActions cdbg_serial_setup;
        std::function<bool()> open_cdbg_port;
        std::function<bool()> cdbg_port_is_open;
        std::function<bool()> target_is_ecu;
        std::function<bool()> use_openport2_adapter;
    };

    struct TestingTag
    {
    };

    LegacyLoggingProtocolFactory(ConstructionDependencies dependencies, TestingTag);

    ConstructionDependencies dependencies_;

    friend class LegacyLoggingProtocolFactoryTestAccess;
};

} // namespace fastecu::desktop::logging
