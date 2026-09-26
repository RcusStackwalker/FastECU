# Step 6e: move platform selection into `apps/desktop`

## Goal

Complete the step 6 bullet "Move platform selection into `apps/desktop`" of
the [modularization plan](../../modularization-plan.md). Step 6c moved
construction of the serial facade into the composition root; the two
platform choices it makes are still inside the serial package:

- **direct vs. remote** — `SerialPortActions` decides in its constructor
  from `peerAddress == ""`;
- **J2534 Unix vs. Windows** — decided by preprocessor guards inside
  `serial_port_actions_direct.{h,cpp}` and `j2534_driver_selection.h`, and
  reaching the application link only transitively through the
  `serial_qt_compat` target.

Success means:

- `SerialPortActions` is a pure marshaling facade: it receives a backend
  factory and knows nothing about direct, remote, J2534, or the peer address.
- The direct/remote rule lives in exactly one place, the desktop composition
  root, through `desktop_serial_factory`.
- No `Q_OS_*`, `WIN32`, or `_WIN64` guard remains in a production source of
  `src/platform/desktop/common/serial/`; the Unix/Windows differences live in
  BUILD-selected source files.
- The `fastecu` binary in `apps/desktop` names the platform's direct backend
  with its own `select()`.
- `serial_port_actions.h` no longer drags the direct backend and the J2534
  headers into every consumer, and `STATUS_SUCCESS`/`STATUS_ERROR` have one
  definition.
- The `serial_qt_compat` allowlist does not grow; it shrinks by one
  (`remote_utility`).
- No wire byte, log string, or call order changes on either OS.

This is also the shape step 7 needs: an Android binary links its own
backend behind the same factory function without editing the serial
package.

## Non-goals

- Draining the UI's ~240 `serial->` calls or deleting `serial_qt_compat`
  (the "Drain the `serial_qt_compat` allowlist" roadmap item).
- A runtime `IJ2534` interface or any other unification of the two `J2534`
  classes, whose APIs differ (Unix drives a serial port; Windows loads a
  vendor DLL through the bridge). That is the separate "Continue separating
  J2534 discovery, PE-bitness/bridge lifecycle…" item in the
  [tech-debt roadmap](../../tech-debt.md).
- Changing `RemoteUtility`'s construction; it is still built for direct
  sessions with an empty peer, as today.
- Moving logging-protocol registration out of `MainWindow`.

## Current state (2026-09-26, `master` at `f965386e`)

**Facade.** `SerialPortActions(QString peerAddress, QString password,
QWebSocket *web_socket, QObject *parent, std::function<SerialBackend *()>
backendFactoryForTests)` (`serial_port_actions.cpp:8`). Without a test
factory it installs one that returns `new SerialPortActionsDirect()` when
`isDirectConnection()` (`peerAddress == ""`), else
`new RemoteSerialBackend(peerAddress, password, externalSocket)`.
`serial_port_actions.h` includes `serial_port_actions_direct.h` and
`websocketiodevice.h` "for consumers that relied on this header's transitive
includes".

**Callers of the facade's platform knowledge.** `MainWindow`
(`mainwindow.cpp:365`) calls `serial->isDirectConnection()` to decide
whether to wait for the remote serial and utility sources, although its
constructor already receives the peer address (`MainWindow w(services,
addr)` in `apps/desktop/main.cpp`).

**Constructor call sites.** Production has two, and both carry the
direct/remote rule:

- `desktop_serial_factory.cpp` (`make_serial_port_actions(peer_address,
  peer_password, log_sink)`), called from `DesktopComposition`;
- `src/platform/desktop/common/transport/desktop_transport_factory.cpp`,
  which builds a facade from `DesktopCanTransportConfig::peer_address` /
  `peer_password` / `backend_factory_for_tests` for `fastecu-bench`
  (`apps/bench/main.cpp`), which never sets a peer.

Tests: about forty sites across thirteen files use the five-argument
constructor with an empty peer and a fake backend factory (`"", "",
nullptr, nullptr, <factory>`, or `nullptr, parent` in
`mainwindow_test.cpp`), and nine default-construct a facade that drives
the real direct backend (`facade_threading_test.cpp` once, never started;
`tests/serial_pty_e2e_test.cpp` once; `tests/tst_mut_dma_integration.cpp`
six times).

**OS guards.** In `serial_port_actions_direct.cpp`: J2534 log-signal
hookup in the constructor and in the J2534 re-create path (Unix only);
`delay(1)` after `PassThruSetProgrammingVoltage` in both LEC-line toggles
(Unix only); J2534 vendor enumeration appended in `check_serial_ports`, the
registry readers, `check_j2534_devices`, and `getAllJ2534DriversNames`
(Windows only, spelled with bare `WIN32`); port-name formation, the
J2534-capable test, and DLL resolution in `open_serial_port`; a commented-out
prefix block; `get_is_tx_done`; serial-port open/close around
`PassThruOpen` and a devID log line in `init_j2534_connection` (Unix only);
`chanID = protocol` after both connect sites (Unix only). In
`serial_port_actions_direct.h`: the `J2534_unix.h`/`J2534_win.h` include, two
identical `protocol = ISO9141` branches, `installed_drivers` and
`getAllJ2534DriversNames` (Windows only). In `j2534_driver_selection.h`:
`isJ2534CapableEntry`'s body. `direct_backend_test.cpp` guards the
`isJ2534CapableEntry` assertions with `Q_OS_UNIX`.

**Facade codes.** `STATUS_SUCCESS 0x00` / `STATUS_ERROR 0x01` are defined in
both `serial_port_actions_direct.h:618` and `src/ui/desktop/dtc_operations.h:55`.
Consumers in `src/ui/desktop`, `common/transport`, and `//tests` reach the
serial copy through the facade header. So do two parameter ids passed
through the facade: `SERIAL_P1_MAX` (a `set_kline_timings` id, defined with
`SERIAL_P1_MIN`…`SERIAL_P4_MAX` in the direct header) and the J2534 IOCTL id
`P1_MAX` (`0x07` in both `J2534_tactrix_*.h`), both used by
`dtc_operations.cpp`.

**Link.** `apps/desktop/BUILD.bazel`'s `fastecu` carries a comment warning
that the J2534 libraries reach the link only through `serial_qt_compat`'s
`select()`, and must be re-added if that target changes.

## Design

Three PRs, stacked with `gh stack`, each branched from its predecessor.

### 6e-1: the direct/remote choice moves to the composition root

**Facade.** The constructor becomes

```cpp
explicit SerialPortActions(std::function<SerialBackend *()> backend_factory, QObject *parent = nullptr);
```

The factory is required; the test seam becomes the only path. Removed:
`peerAddress`, `password`, `externalSocket`, `isDirectConnection()`, and the
default factory. `waitForSource()` stays; `SerialBackend::waitForSource()`
already defaults to a no-op for the direct backend. The header drops
`serial_port_actions_direct.h` and `websocketiodevice.h`; it keeps the
`QtRemoteObjects` module include because the `stateChanged` signal is
declared with `QRemoteObjectReplica::State`.

**Bazel split** (all in `//src/platform/desktop/common/serial`):

- `:serial_qt_compat` keeps `serial_port_actions.*`, `serial_backend_host.*`,
  `serial_backend.h`, and the new `serial_facade_codes.h`. Its deps lose the J2534
  `select()` and `:serial_replicas`. Its visibility list loses
  `//src/platform/desktop/common/remote_utility:__pkg__` and gains nothing;
  `FROZEN` in `scripts/check-serial-compat-allowlist.py` drops the same
  entry.
- New `:direct_serial_backend`: `serial_port_actions_direct.*`, with the
  J2534 `select()`. Depends on `:serial_qt_compat` (for `serial_backend.h`
  and `serial_facade_codes.h`) and `:j2534_driver_selection`.
- New `:j2534_driver_selection`: `j2534_driver_selection.h`, visible to
  this package and `//src/platform/desktop/common/transport`
  (`desktop_transport_factory.cpp` uses `isJ2534CapableEntry`).
- New `:remote_serial_backend`: `remote_serial_backend.*`,
  `websocketiodevice.*`, `qtrohelper.hpp`, `:serial_replicas`.
  `remote_utility` depends on it instead of `:serial_qt_compat`.
- `:direct_serial_backend` and `:remote_serial_backend` are visible to
  `:desktop_serial_factory`, this package's tests, `remote_utility` (remote
  only), and `//tests:__pkg__` for the crash and PTY suites.

**Factory.** `desktop_serial_factory.h`:

```cpp
struct DirectSerial
{
};
struct RemoteSerial
{
    QString address;
    QString password;
};
using SerialConnection = std::variant<DirectSerial, RemoteSerial>;

OwnedSerialPortActions make_serial_port_actions(const SerialConnection& connection, QObject& log_sink);
```

It calls `make_serial_backend_factory(connection)`, also declared in this
header, and passes the result to the facade; the LOG signal wiring is
unchanged. `make_serial_backend_factory` returns the
`std::function<SerialBackend *()>` for the connection, so the choice can be
tested without starting the facade's I/O thread.

**Composition root.** `desktop_composition.h` declares
`SerialConnection serial_connection_from_args(const QString& host, const
QString& password)`: an empty host is `DirectSerial{}`, anything else
`RemoteSerial{host, password}`. `DesktopComposition` calls it. This is the
only place the rule lives.

**Direct backend entry point.** `direct_serial_backend.h` (in
`:direct_serial_backend`) declares `std::unique_ptr<SerialBackend>
make_direct_serial_backend();`, defined in `direct_serial_backend.cpp`.
The factory and the tests that drive the real direct backend use it
instead of naming `SerialPortActionsDirect`.

**Transport factory and bench.** `DesktopCanTransportConfig` drops
`peer_address`, `peer_password`, and `backend_factory_for_tests`, and
gains a required `std::function<SerialBackend *()> backend_factory`; an
empty one fails with `ErrorKind::InvalidConfig`. `fastecu-bench` passes
`make_serial_backend_factory(DirectSerial{})`, so `desktop_serial_factory`
becomes visible to `//apps/bench`. The bench's behavior is unchanged: it
was always direct.

**`MainWindow`.** The splash wait tests the peer address it already
receives (`!peer_address.isEmpty()`) instead of
`serial->isDirectConnection()`. No new `MainWindowServices` field.

**Facade codes.** New `serial_facade_codes.h` in `:serial_qt_compat`
holds what crosses the facade as numbers: `STATUS_SUCCESS`/`STATUS_ERROR`
and `SERIAL_P1_MIN`…`SERIAL_P4_MAX`, moved verbatim from the direct header
and still macros (the Windows SDK's `ntstatus.h` defines `STATUS_SUCCESS`
as a macro, which would break a constexpr of that name in any translation
unit including both), plus `inline constexpr std::uint32_t kJ2534IoctlP1Max
= 0x07;`. `dtc_operations.cpp` passes `kJ2534IoctlP1Max` instead of the
J2534 header's `P1_MAX`, and the direct backend `static_assert`s the two
are equal. The copies in `serial_port_actions_direct.h` and
`dtc_operations.h` are deleted; both include `serial_facade_codes.h`.

**Include-what-you-use.** Every consumer that compiled only through the
facade's transitive includes adds what it uses: `<QSerialPort>` for
`QSerialPort::NoParity` (`menu_actions.cpp`) and `QSerialPort::EvenParity`
(`log_operations_ssm.cpp`), `serial_facade_codes.h` for `STATUS_*` (transport,
`src/ui/desktop`, `//tests`). A consumer needing the direct backend's J2534
types (`tests/tst_serial_port_crash.cpp`) depends on
`:direct_serial_backend` explicitly. The build tells us the full list; the
rule is to add the precise include, never to re-add a transitive one to the
facade header.

**Tests.**

- `desktop_serial_factory_test`: invoking
  `make_serial_backend_factory(DirectSerial{})` yields a
  `SerialPortActionsDirect`, and `RemoteSerial{...}` a
  `RemoteSerialBackend` (checked with `dynamic_cast`, then deleted); the
  existing LOG-wiring cases keep passing through `make_serial_port_actions`.
- `desktop_composition_test`: an empty host builds a direct session; a
  non-empty host a remote one.
- `mainwindow_test`: the remote splash wait is keyed on the peer address.
- `desktop_transport_factory_test`: an empty `backend_factory` fails both
  entry points with `InvalidConfig`.
- The ~40 fake-backend test sites switch to the new constructor, and the
  nine default-constructed ones pass
  `[] { return make_direct_serial_backend().release(); }` (or a factory
  that is never invoked, for the never-started facade).

### 6e-2: the J2534 OS split moves into BUILD-selected sources

**One include path.** Each of `//src/platform/desktop/unix/j2534` and
`//src/platform/desktop/windows/j2534` gains a `j2534_api` target whose only
header, `j2534_api.h`, includes that OS's real `J2534` header and is
published at the shared path `src/platform/desktop/j2534/j2534_api.h`
through `include_prefix`/`strip_include_prefix`. `:direct_serial_backend`
depends on the one matching the OS by `select()`. The guarded include in
`serial_port_actions_direct.h` is replaced by the shared path. Common code
then calls only the API both `J2534` classes provide (`init`, `setDllName`,
`PassThru*`, `J2534_init_ok`); the OS-specific members (`open_serial_port`,
`close_serial_port`, `get_is_tx_done`, `disable`, the Unix LOG signals) are
called only from the matching OS file.

**Per-OS hooks.** Private member functions of `SerialPortActionsDirect`,
declared once in `serial_port_actions_direct.h`, defined in
`serial_port_actions_direct_unix.cpp` or
`serial_port_actions_direct_windows.cpp`, selected by the BUILD:

| Hook | Call site | Unix | Windows |
|---|---|---|---|
| `connect_j2534_logs()` | constructor; J2534 re-create | connect the four LOG signals | no-op |
| `settle_after_programming_voltage()` | both LEC-line toggles | `delay(1)` | no-op |
| `append_j2534_interfaces(QStringList&)` | `check_serial_ports` | no-op | registry vendors, sorted, appended |
| `resolve_port(const QString& entry)` → `{QString port; bool is_j2534;}` | `open_serial_port` | linux prefix, `isJ2534CapableEntry`, split at `" - "` | windows prefix, `is_j2534 = !port.isEmpty()` |
| `select_j2534_dll()` | `open_serial_port`, J2534 branch | no-op | local then installed DLL probe via `check_j2534_devices`, `setDllName` or the "Initializing interface failed!" log |
| `open_j2534_transport()` | `init_j2534_connection`, before `init()` | `j2534->open_serial_port(serial_port) == serial_port` | `true` |
| `close_j2534_transport()` | `init_j2534_connection`, `PassThruOpen` failure | `j2534->close_serial_port()` | no-op |
| `log_j2534_opened()` | `init_j2534_connection`, `PassThruOpen` success | the "INIT: J2534 opened with devID" line | no-op |
| `adopt_j2534_channel_id()` | both connect sites | `chanID = protocol` | no-op |
| `j2534_tx_done()` | `get_is_tx_done` | `j2534->get_is_tx_done()` | `true` |

The registry helpers, `getAllJ2534DriversNames`, and `check_j2534_devices`
move wholesale into the Windows file. `installed_drivers` stays a member on
every platform (empty on Unix) so the header needs no guard. The duplicate
`protocol = ISO9141` branches collapse to one line, and the commented-out
prefix block is deleted. Hook bodies are moved text, not rewritten; each
OS keeps its exact log strings and call order.

**`j2534_driver_selection`.** `isJ2534CapableEntry` gets
`j2534_driver_selection_unix.cpp` / `_windows.cpp` definitions, selected by
`:j2534_driver_selection`'s BUILD; the Qt-only helpers
(`resolveJ2534DllForConnection`, `mergeJ2534DriverViews`) stay inline in the
header.

**Tests** (per [ADR 0005](../../adr/0005-separate-platform-specific-backend-tests.md)):

- `direct_backend_test.cpp`'s guarded `isJ2534CapableEntry` block splits
  into `j2534_driver_selection_unix_test.cpp` and
  `j2534_driver_selection_windows_test.cpp`, each its own target with
  `target_compatible_with`, carrying the same assertions.
- New per-OS hook tests pin the pure hooks through a testable subclass (the
  `tests/tst_serial_port_crash.cpp` pattern): `resolve_port` on sample
  entries, `append_j2534_interfaces` leaving the list untouched on Unix,
  `j2534_tx_done` returning `true` on Windows.
- The existing PTY and crash suites keep covering the Unix open/init path.
  Windows is covered only by the CI build and tests before bench.

**Done check.** `grep -nE 'Q_OS_|\bWIN32\b|_WIN64'` over the production
`.h`/`.cpp` files of `src/platform/desktop/common/serial/` finds nothing.
Test files may keep a small standard-macro guard where ADR 0005 allows it
(the PTY test's `__linux__`).

### 6e-3: the link-time selection moves into `apps/desktop`, plus close-out

**Link seam.**

- New header-only `:direct_serial_backend_api` holds
  `direct_serial_backend.h` (from 6e-1).
- `:direct_serial_backend` splits into `:direct_serial_backend_unix` and
  `:direct_serial_backend_windows`, each holding the common sources, its own
  hook file, `direct_serial_backend.cpp`, its own `j2534_api` dependency,
  and a `target_compatible_with` for its OS.
- `:desktop_serial_factory` depends only on `:direct_serial_backend_api`.
- `apps/desktop` gains an `alias` named `direct_serial_backend` with the
  `select()` between the two implementations, used by `fastecu` and
  `desktop_composition_test`; it replaces the "no signal in this file"
  comment. `apps/bench`'s `fastecu-bench` gets the same `select()`.
- Test targets needing a real direct backend (`desktop_serial_factory_test`,
  `test_direct_backend`, `test_direct_backend_pty`, the `//tests` crash and
  PTY suites) depend on a test-only `:direct_serial_backend_for_tests`
  alias carrying the same `select()`. A target missing the implementation
  fails to link, which is the intended loud failure.

**Close-out docs.**

- [Modularization plan](../../modularization-plan.md): mark 6e complete with
  its PR list; the Status section drops "move platform selection".
- [Tech-debt roadmap](../../tech-debt.md): the allowlist count goes to 5 and
  `remote_utility` leaves the "not debt" note; the duplicate status macros
  are resolved.
- [Design notes](../../design-notes.md): a "Platform selection" section
  recording that the factory owns the direct/remote rule, that per-OS hooks
  sit behind one guard-free header, and that the link-time `select()`
  lives in the binary.
- This spec is distilled into the design notes and deleted, as with 6c and
  6d.

## Behavior changes

None intended. The observable differences are internal: which target
compiles which code, and where the direct/remote rule is evaluated.

## Risk and verification

No wire byte changes, but 6e-2 reorganizes the J2534 open path on both
OSes, and Windows compiles only in CI.

- Every PR passes `bazel test --config=release //...`,
  `prek run --all-files`, and `bazel run //:clang_tidy_report_changed`.
  The full Windows/macOS/Linux CI matrix is mandatory for 6e-2 and 6e-3.
- A new platform-selection bench checklist
  (written in 6e-3) records these entries as **not yet qualified** until run:
  a `fastecu-bench ports`/`connect` smoke, a macOS OpenPort 2.0 connect plus MUT logging
  smoke (the Unix path), and a Windows J2534 connect. A remote-session smoke
  (`--host` against a remote peer) is run if a peer is available and
  recorded as unverified otherwise.
- A rename of a hook or a missed transitive include fails the build rather
  than changing behavior; the risk that remains is a hook body moved to the
  wrong OS file, which the per-OS hook tests and the done-check grep guard.
