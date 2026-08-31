# Support 32-bit-only J2534 DLLs via a bridge helper process

## Problem

FastECU on Windows is a 64-bit application. Its J2534 backend
(`serial_port/J2534_win.cpp`) loads the vendor's PassThru DLL in-process via
`LoadLibrary`/`GetProcAddress`, which requires the DLL to be the same bitness
as the host process. Some J2534 vendor DLLs are 32-bit only and will never be
rebuilt for 64-bit, so FastECU currently cannot use them at all — and can't
even discover them, since `getAllJ2534DriversNames()` only reads
`HKLM\SOFTWARE\PassThruSupport.04.04`, not the WOW6432Node view a 32-bit-only
vendor typically installs into.

A literal "build FastECU as 32-bit too" is not viable: Qt 6 ships no official
32-bit Windows binaries (mingw or MSVC), for either the qmake toolchain or
Bazel's `rules_qt` (which fetches the same official Qt distributions).
Building Qt from source for win32 is unsupported and, per upstream reports,
broken outright since Qt 6.11. This applies equally regardless of build
system, so migrating the release build to Bazel would not remove the
blocker.

## Goal

Let FastECU (still a 64-bit process) drive a 32-bit-only J2534 DLL,
transparently and with no user-facing setting, by running the DLL inside a
small 32-bit helper process and proxying the 14 `PassThru*` calls to it over
IPC.

## Design

### Scope boundary: Windows only

`serial_port/J2534_unix.h`/`.cpp` (Linux/macOS) is a from-scratch
reimplementation of the J2534 API talking directly to OpenPort 2.0 hardware
over `QSerialPort` — it never loads a vendor DLL (`setDllName`/`getDllName`
are explicit no-ops there). There is no bitness mismatch to bridge on
Unix, so it is untouched by this design. `serial_port_actions_direct.h`
already selects the platform implementation at compile time
(`#if defined Q_OS_UNIX ... #elif defined Q_OS_WIN32`), so no new
cross-platform interface is needed — everything below lives inside the
existing Windows-only `J2534` class in `J2534_win.h`/`.cpp`. Its public
surface (the 14 `PassThru*` methods, `setDllName`, `init`, `getLastError`,
etc.) does not change; `SerialPortActionsDirect` requires **no code changes**
and remains unaware a bridge can exist underneath.

### Automatic bitness detection, no new setting

Before loading a DLL, read its PE header (`IMAGE_FILE_HEADER.Machine`) to
determine 32-bit vs. 64-bit — a plain file read, no need to `LoadLibrary`
first:

- 64-bit DLL (the common case today): behavior is unchanged — `J2534` loads
  it in-process exactly as it does now.
- 32-bit-only DLL: `J2534` internally switches to a bridge-proxy backend
  that spawns the helper process and forwards the 14 calls to it. This
  switch is entirely internal to `J2534`; nothing above it changes.

Also fix `getAllJ2534DriversNames()` to merge
`HKLM\SOFTWARE\Wow6432Node\PassThruSupport.04.04` with the existing native
key, so 32-bit-only vendors show up in the adapter list at all — otherwise
they're invisible to a 64-bit process regardless of the bridge.

### Bridge helper process (`j2534_bridge_host`)

- A standalone win32 binary with **no Qt dependency** — plain WinAPI plus
  the existing plain-C struct definitions in `J2534_tactrix_win.h`
  (`PASSTHRU_MSG`, `SCONFIG`, `SCONFIG_LIST`, `SBYTE_ARRAY` are already
  POD). On startup it `LoadLibrary`s the target vendor DLL and resolves the
  same 14 function pointers `J2534_win.cpp` resolves today.
- Spawned lazily (only when the bitness check requires it) via
  `CreateProcess`, stdin/stdout redirected to anonymous pipes — no named
  pipe, no listening socket, nothing else on the machine can find or attach
  to it.
- Immediately assigned to a Job Object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so a FastECU crash or force-kill
  cannot orphan the helper holding the adapter open.
- Only one helper runs at a time, matching the app's existing
  single-adapter-session model.

### IPC protocol

Every call site in `serial_port_actions_direct.cpp` uses exactly one
`PASSTHRU_MSG` in/out per call (`NumMsgs` is always 1) — there are no arrays
to marshal. Each of the 14 `PassThru*` calls becomes a fixed-size frame: a
1-byte function tag plus a fixed request struct and fixed response struct,
`memcpy`'d directly over the pipe (same machine, same endianness — no
serialization library needed).

`PassThruIoctl` takes an untyped `void*` in the real API, but this
codebase's actual usage is a small closed set, each given its own fixed
frame shape rather than a generic blob:

- `SET_CONFIG` — `SCONFIG_LIST` (small bounded array of parameter/value
  pairs)
- `FIVE_BAUD_INIT` / `FAST_INIT` — `SBYTE_ARRAY` (small bounded byte buffer)
- `CLEAR_RX_BUFFER` / `CLEAR_TX_BUFFER` / `CLEAR_MSG_FILTERS` — NULL/NULL
- `READ_VBATT` — NULL in, `unsigned long` out
- `TX_IOCTL_APP_SERVICE` — vendor-specific fixed in/out buffers

### Build: Bazel-only, additive, main app untouched

`FastECU.pro` gets no new entries — the qmake/MinGW build of `FastECU.exe`
is completely unchanged, and remains the release build for the main app.
Bazel's own Qt setup (`rules_qt`, `windows_architecture = win64_msvc2022`)
fetches the same official 64-bit-only Qt distribution qmake does, so
migrating the main app's build to Bazel would not change the bitness
picture either — that migration is explicitly out of scope here (see
below).

The helper has no Qt dependency, which makes it a clean, additive Bazel
target instead: register an x86 Windows C++ platform (MSVC already targets
both x86 and x64 from the same install; `rules_cc` toolchain resolution is
already wired for x64 in this repo) and define `j2534_bridge_host` as a
plain `cc_binary`, built with `/MT` (static CRT, no redistributable
dependency). This avoids hand-writing a `vcvarsall`/`cl.exe`-locating
script and gives proper `bazel test` wiring for the integration tests
below, at the cost of one more `bazel-contrib/setup-bazel` step in jobs
that don't already have Bazel.

### CI wiring

**`pr.yml`:**
- The existing Windows leg of the `bazel` job builds and tests
  `j2534_bridge_host` (+ its `cc_test`) against the registered x86 Windows
  platform.
- The `build` job (qmake path, Windows) gets its own Bazel setup step
  (mirroring the existing pattern of independently re-installing Qt/OpenSSL
  per job) to build the helper before the existing "Build and package
  Windows static (verify only)" step, so PR verification packages the
  helper into the zip exactly as release will.
- Both "verify only" packaging steps (`pr-check-windows.zip`,
  `pr-check-macos.zip`) — previously built and discarded purely to catch
  packaging regressions — are now uploaded via `actions/upload-artifact@v7`
  so a reviewer can download the real build and test it against actual
  hardware (a real 32-bit-only J2534 DLL, or any other change) before
  merging. Artifact names include the PR number (falling back to the
  commit SHA for master-branch pushes); retention is set to 14 days to
  avoid unbounded artifact storage growth. Both platforms are uploaded, not
  just Windows — this is a general pre-merge-testing capability, not
  narrowly tied to this feature.

**`release.yml`:**
- Windows leg gains a Bazel setup + `bazel build --config=release
  --platforms=//bazel:windows_x86 //:j2534_bridge_host` step, then one more
  `copy` line into the existing `dist\` packaging sequence (alongside
  `FastECU.exe` and the OpenSSL DLL) before `windeployqt` runs. `windeployqt`
  does not need to touch the helper (static CRT, no Qt/runtime deps to
  resolve) — it just rides along in the same zip. Net effect: the Windows
  release zip goes from `{FastECU.exe, libcrypto DLL, Qt deploy}` to
  `{FastECU.exe, libcrypto DLL, Qt deploy, j2534_bridge_host.exe}`.

### Error handling

- PE-header read failure (corrupt file, permissions): don't invent a new
  error mode — fall through to the existing direct-load path and let
  today's `LoadLibrary`/`checkDLL` failure reporting handle it.
- Helper fails to spawn (`CreateProcess` failure, missing binary): surfaced
  through the existing `getLastError()`/`reportJ2534Error()` path as a
  clear "bridge helper failed to start" message. No silent fallback to
  direct-load — that would just re-fail with the real bitness mismatch,
  confusingly.
- Vendor DLL fails to load inside the helper: helper returns a structured
  error frame with the OS error text, relayed through the same
  `PassThruGetLastError` plumbing the app already surfaces today.
- Helper crash / broken pipe mid-session: detected on the next read/write,
  treated as a J2534 call failure via the existing error path, connection
  torn down cleanly. No auto-respawn mid-session — that would silently
  lose the vendor DLL's device/channel state, which is worse than a clear
  error.
- Process orphaning: prevented by the Job Object
  (`KILL_ON_JOB_CLOSE`) described above.

## Testing

Bazel-only, Windows-only (the feature doesn't exist on other platforms), no
real vendor DLL or hardware in CI:

- A synthetic fake J2534 DLL (stub implementing the 14 `PassThru*`
  functions with scripted responses), built as an x86 `cc_binary` shared
  library, used only in tests.
- A `cc_test` exercising the helper against that fake DLL over the real IPC
  path — all 14 functions plus the `PassThruIoctl` shapes enumerated above.
- A pure unit test for the PE-bitness detector (feed it x86 and x64 stub
  binaries; no process spawn involved).
- A crash-recovery test: kill the helper process mid-call, assert the
  caller gets a clean error rather than hanging or crashing — same spirit
  as the existing `serial_crash_tests`/`mut_dma_integration_tests`.

Real vendor-DLL / real-hardware validation is a manual gate, not an
automated one — that's what the new PR artifact upload is for.

## Out of scope

- Any change to `J2534_unix`, macOS, or Linux.
- A user-facing setting or toggle — bitness detection and bridging are
  fully automatic.
- Multiple concurrent bridge sessions/adapters.
- Migrating the main app's release build (qmake → Bazel) — Bazel's Qt
  fetch has the identical 64-bit-only limitation, so this would not help
  the 32-bit problem and is a large, separate, higher-risk change to a
  currently-working release pipeline for a shipped product.
- General-purpose plugin/DLL sandboxing beyond this specific J2534 bitness
  bridge.
- Hardware-in-the-loop CI testing.
