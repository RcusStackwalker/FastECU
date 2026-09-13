# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

**This file states rules, not inventories.** Lists that change when code changes — packages, targets, interfaces, qualification status — are deliberately left to the repo and the linked documents, because a copy here goes stale silently. Keep it that way when editing this file: if a line would need updating by a refactor that does not change how work is done, it belongs somewhere else.

## What this is

An independently maintained fork of [miikasyvanen/FastECU](https://github.com/miikasyvanen/FastECU) — a Qt 6 desktop application for reading, flashing, and logging Subaru and Mitsubishi ECUs/TCUs over J2534, K-Line, and CAN. GPLv3.

The fork's ongoing work is a modularization program: making `src/backend` portable (Qt-free, thread-free, filesystem-free) behind injected ports so the core can be reused outside the Qt desktop app. Read the [step-5 umbrella design](docs/superpowers/specs/2026-07-22-step5-backend-portable-design.md) and the [tech-debt roadmap](docs/tech-debt.md) for current priorities before making structural changes.

Two documents carry conventions this file only points at: the [coding style guide](docs/coding-style.md) for how C++ is written here, and the [ADRs](docs/adr/README.md) for structural decisions. Style questions are settled by the guide, not by an ADR.

## Build, test, lint

Bazel — version pinned in `.bazelversion` — is the **only** target graph: application, tests, packaging, coverage, compile commands, and clang-tidy inputs. It needs the Qt 6 host tools and modules named in `.github/workflows/pr.yml`, which is the authoritative environment setup.

```sh
bazel build --config=release //:fastecu                             # app (alias -> //apps/desktop:fastecu)
bazel test  --config=release //...                                  # everything: C++ suites + root build-graph guards
bazel test  --config=release //src/backend/config:app_config_test   # a single test target
bazel test  --config=release //src/backend/protocol:all             # one package's tests
prek run --all-files                                                # clang-format, buildifier, ruff, header and link checks
bazel run //:clang_tidy_report_changed                              # PR gate scope (changed files vs origin/master)
scripts/coverage-local.sh                                           # llvm-cov report -> coverage/ (feeds SonarCloud)
```

The remaining clang-tidy entry points, and how to reproduce the SonarCloud gate locally, are in the style guide's static-analysis section.

Platform-specific targets carry `target_compatible_with` and are skipped on the wrong OS, so `//...` is safe everywhere. Packaging is `scripts/package-macos.sh` and `scripts/package-windows.ps1`; each builds via Bazel first, then collects Qt runtime files.

C++23 (`std::expected`, `std::format`, ranges). MSVC uses `/std:c++latest`; `.bazelrc` pins a macOS minimum OS new enough for `std::format`.

## Layering

Dependencies flow one way: `apps/desktop` → `src/ui` → `src/platform` → `src/backend` → `src/algorithms`. `platform → backend` is permitted — platform implements backend-owned interfaces — and the reverse never is.

- **`src/algorithms/`** — pure, Qt-free logic: protocol codecs, checksum, expression evaluation, diagnostics. A couple of packages carry a `qt_compat` subpackage shimming Qt types for legacy callers; don't add new ones.
- **`src/backend/`** — use cases and domain model. Most targets here are **portable**: no Qt, no threads, no filesystem.
- **`src/backend/ports/`** — the injected-port interfaces, plus `Result<T>` and `Error`. Transport ports (`IKlineTransport`, `ICanTransport`, `ISsmTransport`) deliberately stay in `src/backend/protocol/` instead.
- **`src/platform/desktop/`** — the Qt and OS adapters: an implementation of every port, J2534 and serial, the worker threads.
- **`src/ui/desktop/`** — widgets, `.ui` forms, `MainWindow`, dialogs, bundled hex editor.
- **`src/backend/definitions/`** — the legacy `FileActions` god object, being decomposed into use cases. Distinct from the newer `src/backend/definition/`; add to that one, not this one.

### Error and byte conventions

- Backend operations return `fastecu::Result<T>` (`std::expected<T, Error>`), checked with `.has_value()` and never the implicit `operator bool`. **Exceptions never cross a port.** The `ErrorKind` set is closed — don't add a value without amending the step-5 design doc.
- Pure protocol, checksum, logging, and flash logic uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView` from `src/algorithms/protocol/bytes.h`. `QByteArray` is a boundary type only, converted explicitly via `qt_bytes.h`.

## Build-graph guardrails

The root package holds guards for invariants the compiler cannot see. They fail CI, not your editor, and each one's script in `scripts/` states the rule it enforces — read that before working around a failure.

- **`//:portable_closure`** — no `//src/platform` label may be reachable from a portable target. It fails `bazel build`, not the test suite. Register new portable targets in `PORTABLE_PACKAGES` (`bazel/portable_targets.bzl`). Qt needs no guard: it is unreachable from a portable package by construction.
- **Ratchet lists only shrink.** Some guards freeze a list of remaining transitional debt — the `serial_qt_compat` visibility list, the flash families still calling `SerialPortActions` directly. Entries come out as the work lands; **an entry may never go in.** Needing to add one means the change took the legacy path and should be rewritten to take the portable one.
- Windows 32-bit J2534 vendor DLLs are reached through an out-of-process bridge under `src/platform/desktop/windows/j2534/`; the x86 host binary is built in-graph via the platform transition in `bazel/x86_windows_transition.bzl`.

## Writing targets and tests

- Tests are **package-owned and co-located** with the code (`foo.cpp` + `foo_test.cpp` in the same package). `tests/` holds only cross-package integration and platform harness tests.
- Use `fastecu_portable_gtest` (deliberately Qt-free closure) or `fastecu_gtest` (links `QT_DEPS_NO_WIDGETS`) from `bazel/gtest_targets.bzl`. QtTest-style suites needing moc use `fastecu_qttest`, loaded from `bazel/qt_targets.bzl` in the widget layer and from `bazel/qt_common.bzl` below it — `qt_targets.bzl` is visibility-restricted so backend and algorithms cannot reach Qt Widgets through it.
- Mocks and fakes are package-owned: the package defining an interface adds a `testing/` subpackage with one `cc_library(testonly = True)` per mock, each with its own test. `src/backend/ports/testing/` is the reference.
- `qt_cc_library` lists moc'd headers in `hdrs` and everything else in `normal_hdrs` — a `Q_OBJECT` header missing from `hdrs` links but fails at runtime.
- Platform differences go in separate source files selected by the BUILD file, not `#ifdef` branches inside a shared source. Where a preprocessor guard is unavoidable, spell it `_WIN32` (or `Q_OS_WIN32`); a bare `WIN32` is rejected, because Bazel does not define it and the branch would silently take the POSIX path on Windows.
- Cross-document references in Markdown are links with human-readable text, not backticked paths — lychee checks links under `prek` and cannot see a path written as inline code.

## Hardware-facing caution

Flash and logging paths talk to real ECUs, and a wrong write bricks hardware. What has been qualified, and on what bench, is recorded in the [flash qualification matrix](docs/flash-qualification-matrix.md) and the bench checklists in `docs/` — among them the [bench CLI qualification checklist](docs/bench-cli-checklist.md), which gates `//apps/bench:fastecu-bench` before its first use against a real ECU. Consult those before assuming a path is safe, and treat anything they do not record as qualified as experimental. Never relax an address-window guard, and never mark a path qualified without a checklist entry behind it.

## Git workflow

Work lands through pull requests: branch, commit, push, open a PR. `prek` refuses commits made directly on `master`.
