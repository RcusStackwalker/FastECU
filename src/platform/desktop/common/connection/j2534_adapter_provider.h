#pragma once

#include "src/platform/desktop/common/connection/local_adapter.h"
#include "src/platform/desktop/common/logging/cdbg_serial_setup.h"
#include "src/platform/desktop/common/serial/serial_backend.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

class SerialPortActions;

namespace fastecu::desktop::connection
{

namespace detail
{
enum class J2534DiscoverySource
{
    UnixSerialPorts,
    WindowsRegistryAndSerialPorts,
};
} // namespace detail

class J2534AdapterProviderTestAccess;

class J2534AdapterProvider final : public ILocalAdapterProvider
{
  public:
    J2534AdapterProvider();

    dashboard::AdapterKind kind() const override;
    Result<std::vector<LocalAdapterDescriptor>> discover() override;
    Result<std::unique_ptr<OpenedCanAdapter>> open(std::string_view candidate_id,
                                                   const dashboard::CdbgConnectionProfile& profile) override;

  private:
    struct Dependencies
    {
        detail::J2534DiscoverySource discovery_source;
        std::function<QStringList(SerialPortActions&)> list;
        std::function<std::unique_ptr<SerialPortActions>()> construct;
        std::function<Status(SerialPortActions&, const logging::RawCanSetupProfile&)> configure;
        std::function<J2534RawCanOpenResult(SerialPortActions&)> open;
    };

    struct TestingTag
    {
    };

    J2534AdapterProvider(Dependencies dependencies, TestingTag);

    Dependencies dependencies_;
    std::unordered_map<std::string, QString> issued_entries_;

    friend class J2534AdapterProviderTestAccess;
};

} // namespace fastecu::desktop::connection
