#pragma once

#include <memory>

#include "src/platform/desktop/common/connection/adapter_connection.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

class SerialPortActions;

namespace fastecu::desktop::connection::testing
{

// A facade over a NiceFakeBackend with an AdapterConnection on top. UI tests
// drive MainWindow through it without including serial_port_actions.h, which
// the UI package can no longer see.
class AdapterConnectionHarness
{
  public:
    AdapterConnectionHarness();
    ~AdapterConnectionHarness();

    AdapterConnectionHarness(const AdapterConnectionHarness&) = delete;
    AdapterConnectionHarness& operator=(const AdapterConnectionHarness&) = delete;

    // Null if the fake backend failed to start.
    FakeBackend *Fake() const
    {
        return fake_;
    }

    AdapterConnection& Connection()
    {
        return *connection_;
    }

  private:
    FakeBackend *fake_ = nullptr;
    std::unique_ptr<SerialPortActions> serial_;
    std::unique_ptr<AdapterConnection> connection_;
};

} // namespace fastecu::desktop::connection::testing
