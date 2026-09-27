#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"

// A SerialPortActions facade whose backend is a Google Mock FakeBackend,
// already created by the time the constructor returns.
//
// The facade creates its backend lazily, on the first call it marshals to the
// I/O thread. A bare factory lambda therefore leaves the fake pointer null
// until some unrelated setter happens to run -- the
// `set_add_ssm_header(false); // forces backend creation` line this fixture
// replaces. Making that call here is what lets fake() be a reference instead
// of a pointer every test has to null-check.
//
// Mock defaults to NiceFakeBackend. Name a StrictMock instead when the test is
// about calls that must not happen, and pass `arrange` to install the
// expectations that construction itself will trip -- it runs on the new
// backend before the facade uses it.
//
// Lifetime: the fixture must outlive the facade, released or not, because the
// factory it installed writes back into the fixture. Declaring the consumer
// after the fixture is enough, and is what the transport suites do.
template <typename Mock = NiceFakeBackend> class FakeBackedSerial
{
  public:
    using Arrange = std::function<void(Mock&)>;

    FakeBackedSerial() : FakeBackedSerial(Arrange{})
    {
    }

    explicit FakeBackedSerial(Arrange arrange)
        : serial_(std::make_unique<SerialPortActions>(
              [this, arrange = std::move(arrange)]() -> SerialBackend *
              {
                  auto *fake = new Mock();
                  if (arrange)
                  {
                      arrange(*fake);
                  }
                  fake_ = fake;
                  return fake;
              }))
    {
        // Any marshaled call forces the backend into existence; this one is
        // otherwise inert. It is the call a StrictMock fixture must arrange.
        serial_->set_add_ssm_header(false);
    }

    // Non-copyable and non-movable: the installed factory captures `this`.
    FakeBackedSerial(const FakeBackedSerial&) = delete;
    FakeBackedSerial& operator=(const FakeBackedSerial&) = delete;

    Mock& fake()
    {
        return *fake_;
    }

    SerialPortActions& operator*()
    {
        return *serial_;
    }

    SerialPortActions *operator->()
    {
        return serial_.get();
    }

    SerialPortActions *get()
    {
        return serial_.get();
    }

    // Hands the facade to its consumer. fake() keeps working afterwards: the
    // backend now belongs to whoever took the facade, and dies with it.
    std::unique_ptr<SerialPortActions> release()
    {
        return std::move(serial_);
    }

    // Destroys the facade, and with it the backend, while the fixture stays in
    // scope. The non-owning transport tests use this to choose the moment
    // teardown happens.
    void reset()
    {
        serial_.reset();
    }

  private:
    Mock *fake_ = nullptr;
    std::unique_ptr<SerialPortActions> serial_;
};

// A closure carries no information about Mock (it is not a std::function), so
// an arranged fixture that does not name its mock type gets the default.
template <typename F> FakeBackedSerial(F) -> FakeBackedSerial<NiceFakeBackend>;
