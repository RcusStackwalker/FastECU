# Google Mock reference: serial backend

The package-owned [FakeBackend](../src/platform/desktop/common/serial/testing/fake_backend.h)
is a Google Mock implementation of the serial backend. It keeps the QObject
identity needed by the real serial facade and delegates configuration storage
to the direct backend. Hardware operations have inert defaults. Tests describe
their required interactions with `EXPECT_CALL`, instead of adding response flags,
exception switches, or string call logs to the shared backend.

Start with the small [backend tests](../src/platform/desktop/common/serial/testing/fake_backend_test.cpp).
The [facade tests](../src/platform/desktop/common/serial/facade/facade_threading_test.cpp)
show exceptions and coordinated concurrent callers; the
[CAN adapter tests](../src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp)
show ordered configuration and stopping at the first failure.

## Setting expectations

Create a `NiceFakeBackend` through the facade's backend factory. The facade
creates it lazily, so complete a synchronous configuration call before setting
expectations. Set up expectations before starting worker calls, then join those
calls before verifying or destroying the mock. The facade owns the backend.

The transport suites do all of that through the
[FakeBackedSerial fixture](../src/platform/desktop/common/serial/testing/fake_backed_serial.h),
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
  The [FakeBackedSerial fixture test](../src/platform/desktop/common/serial/testing/fake_backed_serial_test.cpp)
  uses `StrictMock<FakeBackend>` to forbid every call the test has not arranged.
- Use `DoDefault()` when an expected setter must also update configuration.
  `Return(true)` reports success but replaces that default state update.
- Use `Throw(...)` for driver failures and lambda actions with synchronization
  primitives for calls that must block. Keep scenario state local to the test.

## Qt application integration

Every C++ suite uses GoogleTest, so Google Mock failures contribute to the
executable's exit status. Register a `CoreApplicationEnvironment` or
`WidgetsApplicationEnvironment` from the
[test support package](../src/platform/desktop/common/testing/BUILD.bazel)
with `testing::AddGlobalTestEnvironment`. Application arguments remain owned
until teardown, and the application outlives fixtures. Use the environment's
pre-construction callback for application attributes.

Record signals with the typed `SignalRecorder`; inspect copied tuples from
`snapshot()` after checking `count()`. Worker emissions are captured directly
under synchronization. Keep thread joins and gates that establish completion.
Use `wait_until` for queued events and `process_events_for` when checking that
something remains absent for a deadline. Assert on the returned condition at
the call site, and use `ASSERT_NO_FATAL_FAILURE` when a helper's fatal failure
must stop dependent operations.

Exceptional process runners use `use_gtest_main = False`, initialize Google
Mock, and return `RUN_ALL_TESTS()`. Join workers and destroy mocks before
returning. The backend suite verifies failing exit status for unmet calls,
forbidden calls, and wrong arguments through the real facade's I/O thread.

Depend on `//src/platform/desktop/common/serial/testing:fake_serial_backend`.
Its `@googletest//:gtest` dependency supplies both GoogleTest and Google Mock.
Run the reference with:

```sh
bazel test --config=release //src/platform/desktop/common/serial/testing:fake_backend_test
```

## Scripted cancellation and boundary probes

`FakeCancellationToken::cancel_on_check(n)` counts all cancellation queries,
including queries in `FakeClock::sleep()` and scripted transport reads. Trace
helper calls when choosing a checkpoint; a mistaken checkpoint does not justify
changing production cancellation order.

When probing whether production UI can include a serial facade header, put the
probe at the end of the source. A top-of-file probe can instead fail on the
`STATUS_SUCCESS` macro collision with a definition-conversion enum and prove
nothing about the missing compile input.
