#include "src/platform/desktop/common/connection/testing/adapter_connection_harness.h"

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

namespace fastecu::desktop::connection::testing
{

AdapterConnectionHarness::AdapterConnectionHarness()
    : serial_(std::make_unique<SerialPortActions>(
          [this]() -> SerialBackend *
          {
              auto *fake = new NiceFakeBackend;
              fake_ = fake;
              return fake;
          }))
{
    // The facade creates its backend on the first marshaled call; this one
    // is otherwise inert.
    if (!serial_->SetAddSsmHeader(false))
    {
        fake_ = nullptr;
    }
    connection_ = std::make_unique<AdapterConnection>(*serial_);
}

AdapterConnectionHarness::~AdapterConnectionHarness()
{
    connection_.reset();
    serial_.reset();
}

} // namespace fastecu::desktop::connection::testing
