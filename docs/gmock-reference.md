# Google Mock reference: serial backend

The package-owned [FakeBackend](../src/platform/desktop/common/serial/testing/fake_backend.h)
is a Google Mock implementation of the serial backend. It keeps the QObject
identity needed by the real serial facade and delegates configuration storage
to the direct backend. Hardware operations have inert defaults. Tests describe
their required interactions with `EXPECT_CALL`, instead of adding response flags,
exception switches, or string call logs to the shared backend.

Start with the small [backend tests](../src/platform/desktop/common/serial/testing/fake_backend_test.cpp).
The [facade tests](../src/platform/desktop/common/serial/facade_threading_test.cpp)
show exceptions and coordinated concurrent callers; the
[CAN adapter tests](../src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp)
show ordered configuration and stopping at the first failure.

## Setting expectations

Create a `NiceFakeBackend` through the facade's backend factory. The facade
creates it lazily, so complete a synchronous configuration call before setting
expectations. Set up expectations before starting worker calls, then join those
calls before verifying or destroying the mock. The facade owns the backend.

The transport suites do all of that through the
[FakeBackedSerial fixture](../src/platform/desktop/common/transport/fake_backed_serial.h),
which performs the lazy-creation call in its constructor, so `fake()` is a
reference that is valid immediately. Name a `StrictMock` as its template
argument, and pass its `arrange` callback the expectations that construction
itself will trip. Suites outside that package pass a factory to the facade
directly.

```cpp
FakeBackedSerial serial;

::testing::InSequence sequence;
EXPECT_CALL(serial.fake(), write_serial_data(QByteArray("request")))
    .WillOnce(::testing::Return(QByteArray("request")));
EXPECT_CALL(serial.fake(), read_serial_data(50))
    .WillOnce(::testing::Return(QByteArray("reply")));
```

- `ON_CALL` supplies default behavior; it does not require a call.
- `EXPECT_CALL` checks arguments and call counts. Use `InSequence` when order
  is part of the contract. For successive results from the same method, chain
  `WillOnce` actions or put the expectations in a sequence; later matching
  expectations otherwise take precedence.
- `NiceFakeBackend` allows uninteresting calls. Explicitly forbid operations
  with `.Times(0)` when testing cancellation or failure short-circuiting.
  The [legacy TCU preflight test](../src/ui/desktop/flash/tcu/flash_tcu_subaru_denso_sh705x_can_test.cpp)
  uses `StrictMock<FakeBackend>` to forbid every call after initial setup.
- Use `DoDefault()` when an expected setter must also update configuration.
  `Return(true)` reports success but replaces that default state update.
- Use `Throw(...)` for driver failures and lambda actions with synchronization
  primitives for calls that must block. Keep scenario state local to the test.

## QtTest integration

GoogleTest runners report GMock failures automatically. QtTest runners do not.
These suites initialize Google Mock before Qt and combine both failure states:

```cpp
::testing::InitGoogleMock(&argc, argv);
QCoreApplication application(argc, argv);
ExampleTest test;
const int result = QTest::qExec(&test, argc, argv);
return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
```

Use `QApplication` for widget suites. Destroy mocks and join their worker
threads before checking the final status. A QtTest `PASS` line alone does not
prove GMock expectations passed; inspect the process exit status and GMock
diagnostics. The backend test includes subprocess checks for unmet calls,
forbidden calls, and wrong arguments through the real facade's I/O thread.

Depend on `//src/platform/desktop/common/serial/testing:fake_serial_backend`.
Its `@googletest//:gtest` dependency supplies both GoogleTest and Google Mock.
Run the reference with:

```sh
bazel test --config=release //src/platform/desktop/common/serial/testing:fake_backend_test
```
