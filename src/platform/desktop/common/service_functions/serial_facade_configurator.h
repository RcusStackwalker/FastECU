#pragma once

#include "src/backend/ports/result.h"
#include "src/backend/service_functions/service_function_types.h"

class SerialPortActions;

namespace fastecu::service_functions
{

// Applies a session's SsmTransportConfig to a serial facade. Behind an
// interface so the worker's "setup failure touches no hardware" contract is
// assertable without a real SerialPortActions.
class ISerialFacadeConfigurator
{
  public:
    virtual ~ISerialFacadeConfigurator() = default;
    virtual Status apply(const SsmTransportConfig& config) = 0;
};

class SerialPortActionsConfigurator final : public ISerialFacadeConfigurator
{
  public:
    explicit SerialPortActionsConfigurator(SerialPortActions *serial) : serial_(serial)
    {
    }

    Status apply(const SsmTransportConfig& config) override;

  private:
    SerialPortActions *serial_;
};

} // namespace fastecu::service_functions
