# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An independently maintained fork of [miikasyvanen/FastECU](https://github.com/miikasyvanen/FastECU) — a Qt 6 desktop application for reading, flashing, and logging Subaru and Mitsubishi ECUs/TCUs over J2534, K-Line, and CAN. GPLv3.

The fork's ongoing work is a modularization program: making `src/backend` portable (Qt-free, thread-free, filesystem-free) behind injected ports so the core can be reused outside the Qt desktop app. Read the [step-5 umbrella design](docs/superpowers/specs/2026-07-22-step5-backend-portable-design.md) and the [tech-debt roadmap](docs/tech-debt.md) for current priorities before making structural changes.

## Build, test, lint

Bazel 9.1.1 (pinned in `.bazelversion`) is the **only** target graph — application, tests, packaging, coverage, compile commands, and clang-tidy inputs (ADR 0001, ADR 0007). Requires Qt 6.8.3 host tools (Charts, SerialPort, RemoteObjects, WebSockets) and OpenSSL. `.github/workflows/pr.yml` is the authoritative environment setup.

```sh
bazel build --config=release //:fastecu                      # app (alias -> //apps/desktop:fastecu)
bazel test  --config=release //...                           # everything: C++ suites + root guard tests
bazel test  --config=release //src/backend/config:app_config_test   # a single test target
bazel test  --config=release //src/backend/protocol:all      # one package's tests
prek run --all-files                                         # clang-format, buildifier, ruff, pragma-once (ADR 0002)
bazel run //:clang_tidy_report_changed                       # PR gate scope (changed files vs origin/master); needs system LLVM on PATH
bazel run //:clang_tidy_fix_changed                          # same scope, applies fixes; macOS/Linux only
bazel run //:clang_tidy_report                               # full-repo sweep, advisory/manual only; needs system LLVM on PATH
bazel run //:clang_tidy_fix                                  # full-repo sweep, macOS/Linux only; needs clang-apply-replacements
scripts/coverage-local.sh                                    # llvm-cov report -> coverage/ (feeds SonarCloud)
```

Test targets live under `//src`, `//tests`, `//resources`, and the root package (build-graph guards); use `//...` to run them all. Platform-specific targets carry `target_compatible_with` and are skipped on the wrong OS.

Packaging: `scripts/package-macos.sh`, `scripts/package-windows.ps1` — both build via Bazel first, then collect Qt/OpenSSL runtime files.

C++23 (`std::expected`, `std::format`, ranges). MSVC uses `/std:c++latest`; macOS pins `--macos_minimum_os=26.0` for `std::format`.

## Layering

Dependencies flow one way: `apps/desktop` → `src/ui` → `src/platform` → `src/backend` → `src/algorithms`. `platform → backend` is permitted (platform implements backend-owned interfaces); the reverse never is.

- **`src/algorithms/`** — pure, Qt-free logic: protocol codecs (`ssm/`, `mut_dma/`, `colt/`), checksum, crypto, expression evaluation, diagnostics. Each has a sibling `:qt_compat` shim target for legacy callers; don't add new ones.
- **`src/backend/`** — use cases and domain model: `protocol/` (transport interfaces + drivers), `flash/` (plan/validate/execute + `eeprom/`), `logging/`, `definition/` (RomRaider + EcuFlash parsers), `calibration/`, `config/`, `checksum/`, and `ports/`. Most targets here are **portable** — no Qt, no threads, no filesystem.
- **`src/backend/ports/`** — the injected-port interfaces: `IClock`, `ICancellationToken`, `IEventSink`, `IFileRepository`, `IFileSystem`, `ISettings`, `IResourceBundle`, plus `Result<T>`/`Error` in `result.h`/`error.h`. Transport ports (`IKlineTransport`, `ICanTransport`, `ISsmTransport`) deliberately stay in `src/backend/protocol/`.
- **`src/platform/desktop/`** — Qt/OS adapters: `common/ports/` (Qt implementations of every port), `common/serial/` (J2534 + serial, threading facade), `common/transport/`, `common/logging/` + `common/flash/` (worker threads), `unix/j2534/`, `windows/j2534/`.
- **`src/ui/desktop/`** — widgets, `.ui` forms, `MainWindow`, flash dialogs, bundled hex editor.
- **`src/backend/definitions/`** — legacy `FileActions` god object (distinct from the new `src/backend/definition/`), being decomposed into use cases.

### Error and byte conventions

- Backend operations return `fastecu::Result<T>` (`std::expected<T, Error>`) with one of seven `ErrorKind` values. **Exceptions never cross a port.** Don't add `ErrorKind` values without amending the step-5 design doc.
- Pure protocol/checksum/logging/flash logic uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView` from `src/algorithms/protocol/bytes.h`; `QByteArray` is a boundary type only, converted explicitly via `qt_bytes.h` (ADR 0004).

## Build-graph guardrails

These exist because the compiler can't catch them; they fail CI, not your editor.

- **`//:portable_closure`** — resolves the transitive closure of every portable target and rejects Qt or JNI deps. Adding a Qt dep to a portable backend target fails here even if it compiles. New portable targets must be registered in both the `genquery` in `BUILD.bazel` and `PORTABLE_ROOTS` in `scripts/check-portable-closure.py`.
- **`//:serial_compat_allowlist`** — freezes the visibility list of `//src/platform/desktop/common/serial:serial_qt_compat`. That list is transitional debt: **it may shrink, never grow.**
- **`//:openpty_includes`** — platform-specific backend tests live in separate source files listed in `*_UNIX_SRCS` / `*_WIN32_SRCS`, not behind `#ifdef` in common sources (ADR 0005).
- **`//:bazel_openssl_wiring`** — keeps `MODULE.bazel`, `pr.yml`, and the crypto BUILD file consistent.
- Windows 32-bit J2534 vendor DLLs are reached through an out-of-process bridge (`src/platform/desktop/windows/j2534/j2534_bridge_*`); the x86 host binary is built in-graph via the platform transition in `bazel/x86_windows_transition.bzl`.

## Writing targets and tests

- Tests are **package-owned and co-located** with the code (`foo.cpp` + `foo_test.cpp` in the same package). `tests/` holds only cross-package integration and platform harness tests.
- Use `fastecu_portable_gtest` (Qt-free closure) or `fastecu_gtest` (links `QT_DEPS`) from `bazel/gtest_targets.bzl`; `fastecu_qttest` in `bazel/qt_targets.bzl` for QtTest-style suites needing moc.
- Mocks/fakes are package-owned: a package defining an interface adds a `testing/` subpackage with one `cc_library(testonly = True)` target per mock, each with its own test (ADR 0008; `src/backend/ports/testing/` is the reference).
- Qt targets list moc'd headers explicitly in a `MOC_HDRS` list and everything else in `normal_hdrs` — a `Q_OBJECT` header missing from `MOC_HDRS` links but fails at runtime.
- Prefer `std::string_view` by value over `const char*` / `const std::string&` (ADR 0009), gmock matchers for property assertions (ADR 0010), `std::format` for message construction (ADR 0011), and ranges/views over index loops (ADR 0012).
- Every header needs `#pragma once` (enforced by prek).
- Cross-document references in Markdown are links with human-readable text, not backticked paths — lychee (via prek) checks links and cannot see a path written as inline code.

## Hardware-facing caution

Flash and logging paths talk to real ECUs; a wrong write bricks hardware. Anything not yet bench-qualified is documented as such — see the [flash qualification matrix](docs/flash-qualification-matrix.md) and the bench checklists in `docs/`. The `MUT_DMA` protocol (Mitsubishi M32R K-Line) is **experimental and not bench-qualified**; its wake sequence and memory writes are unverified on a live ECU. Don't relax an address-window guard or mark a path qualified without a checklist entry backing it.
