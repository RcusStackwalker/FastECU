# Holistic bounds-check hardening for FastECU

**Date:** 2026-06-09
**Status:** Approved, ready for implementation plan

## Problem

FastECU parses untrusted input — ECU responses over serial/CAN/K-Line, and
ROM/definition files — by indexing `QByteArray` buffers directly. There are
~2300 `.at()` calls across the codebase, concentrated in the flash and logging
modules. Each module hand-rolls its own length validation, and the guards drift
out of sync with the accesses:

- `log_operations_ssm.cpp:369` accessed `received.at(0/1/2)` with **no** length
  guard. When the ECU sent no data, `received` was empty and `at(0)` on an empty
  Qt6 `QByteArray` dereferenced a null data pointer → `SIGSEGV` (the crash that
  motivated this work; fixed separately by adding a `length() >= 3` guard).
- `flash_ecu_subaru_denso_sh7058_can.cpp` guards a block with `length() > 5`
  then later does `seed.append(received.at(6..9))`, which needs `length() >= 10`.

The release build defines `QT_NO_DEBUG`, which strips the `Q_ASSERT` that Qt's
`.at()` already contains — so an overrun is silent undefined behavior (garbage
read or null deref) instead of a diagnosed failure.

**Goal:** a holistic, low-churn mechanism that turns every out-of-bounds access
into an immediate, logged, located abort rather than silent corruption — across
the whole codebase, not site-by-site.

## Key facts that shape the design

- The code has **zero** raw-pointer indexing (`constData()[i]` / `data()[i]`)
  and only 14 `operator[]` sites. Virtually all buffer access goes through Qt's
  `.at()`.
- Qt's `QByteArray::at()` / `operator[]` already contain
  `Q_ASSERT(size_t(i) < size_t(size()))`. The assert is compiled out only
  because of `QT_NO_DEBUG`.
- Defining `QT_FORCE_ASSERTS` re-activates `Q_ASSERT` even under `QT_NO_DEBUG`.
  On failure, Qt's `qt_assert` prints
  `ASSERT: "i < size()" in file X, line N` and aborts.

This means the bulk of the "all over the code" surface can be hardened with one
build flag and no source changes.

## Behavior contract (decided)

- **On overrun:** log + abort the process loudly with file/line diagnostics
  (fail-fast). Chosen over "return sentinel 0 and continue" (risks acting on
  garbage flash bytes) and "abort the operation, keep app alive" (would require
  threading error returns through every parser).
- **Where active:** all builds — dev, CI, and shipped release. Users get a
  diagnosed abort instead of a silent crash.

## Components

### 1. Primary safety net: forced Qt assertions (all builds)

Add to `FastECU.pro`, unconditionally:

```pro
DEFINES += QT_FORCE_ASSERTS
```

Re-arms `Q_ASSERT` for every `QByteArray` / `QString` / `QList` `.at()` and
`operator[]`. Overruns become loud, located aborts. No source changes.

### 2. Sanitizer build for the test suite (gap closer)

Forced asserts only see Qt containers. Add an opt-in qmake config to catch raw
C arrays, `std::` access, use-after-free, and freed-memory reads via `.mid()`:

```pro
CONFIG(sanitize) {
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
    QMAKE_LFLAGS   += -fsanitize=address,undefined
}
```

Wire `CONFIG+=sanitize` into the existing PTY-mock integration tests under
`tests/` so CI runs the suite under ASan+UBSan.

### 3. Burn-in / verification pass (the bulk of the work)

Turning `QT_FORCE_ASSERTS` on globally converts *latent over-reads* — paths that
currently index past the buffer but happen not to crash — into aborts. This is
intended, but must be shaken out before relying on it:

1. Build with the flag; run the full `tests/` suite (serial-crash and MUT/DMA
   integration tests included).
2. Exercise the main device paths against the PTY mock: log polling and the
   flash-module response parsers with their known-shaky `length() > N` guards.
3. Each assert that fires is a real latent bug → fix the guard at that site so
   the `length()` check matches the actual maximum index accessed.
4. Add a deliberate regression test feeding a truncated response and asserting
   the operation rejects it cleanly.

### 4. Optional targeted diagnostics (deferred — not in this spec)

A `byte_at(buf, i)` helper emitting rich context ("ECU returned 4 bytes,
expected >= 10") would beat a bare assert for triage, but it is incremental
churn across the flash modules. Deferred as a follow-up to layer onto hot paths
only if bare-assert messages prove too thin. YAGNI for now.

## Accepted trade-off

A forced abort mid-flash could interrupt an ECU write. Accepted: continuing to
act on garbage/truncated bytes (the status quo) is more dangerous than stopping,
and flash protocols recover via boot mode on next launch. Documented as a
deliberate choice.

## Scope

- **In:** device-input + file-input hardening via the global net
  (Components 1–3).
- **Out:** per-call-site rewrites (Component 4); the centralized response-framing
  refactor (the long-term ideal, tracked separately).
