# FastECU Modularization and Android-Readiness Plan

## Status

The Bazel migration prerequisite is complete. Commit `66dd62e` removed qmake,
and ADR 0001 records Bazel as the sole build graph for the application, tests,
release packaging, SonarCloud inputs, and coverage. This plan starts from that
post-migration state; it does not include another build-system cutover or any
qmake/Bazel synchronization work.

The post-migration baseline characterization is complete. PR
[#46](https://github.com/RcusStackwalker/FastECU/pull/46) merged the exact
reviewed head `3ce7c3d` into `master` as `08e7d194` on 2026-07-19. It converted
the 13 approved headless test labels to GoogleTest and added checksum, parser,
ROM-decoding, and flash-planning characterization without changing production
source, source layout, packaging, workflows, or `docs/coverage-baseline.txt`.

Mechanical source and target modularization has not started. The current tree
still uses root-level application sources plus `logging/`, `modules/`,
`protocol/`, and `serial_port/`; the planned `apps/`, `src/`, and `resources/`
trees do not exist yet. Step 2 is the next implementation phase.

## Verified Current Baseline

Verified on 2026-07-19 against PR #46 head `3ce7c3d`, merged into `master` as
`08e7d194`:

- Bazel 9.1.1 with Bzlmod is the only active project build graph. No qmake
  project files remain.
- Root `BUILD.bazel` owns `//:fastecu`, the aggregate
  `//:fastecu_core_common`, the Unix and Windows platform libraries, Qt
  resources/forms/replicas, and a few standalone support libraries.
- `bazel/fastecu_sources.bzl` remains the central application and test source
  manifest. `//:fastecu` is still the concrete desktop binary, not an alias to
  a package-owned application target.
- GoogleTest 1.17.0.bcr.2 is declared for the headless lane. The 13 approved
  bytes, expression, MUT/DMA, SSM, CDBG, and Colt labels retain their original
  Bazel names and now run without QtTest ownership; temporary parity targets
  and obsolete Qt runners are removed.
- Characterization covers exact synthetic outputs for all nine checksum
  families, application/logger/RomRaider/EcuFlash/diagnostic parsing, public
  ROM decoding across widths/byte order/scaling/X-Y address order, all flash
  lookup tables, and read/write/test-write pre-I/O behavior.
- Accepted baseline limitations remain explicit: several legacy
  driver/codec/freeform and CDBG/Colt expectations reuse production helpers or
  constants, and macOS message-box tests verify exact text but not the native
  title. Replace the coupled expectations when the corresponding boundaries
  become injectable; do not silently reinterpret the frozen compatibility
  quirks.
- Local release verification built 74 targets, passed all 22 direct
  risk-surface tests, and passed 34 applicable tests with the five expected
  Windows-only targets skipped on macOS. Exact-head PR workflow
  [29678865967](https://github.com/RcusStackwalker/FastECU/actions/runs/29678865967)
  recorded the three-OS build/test matrix, Windows/macOS package verification,
  pre-commit, coverage/SonarCloud, and the existing non-blocking clang-tidy
  reporting path.
- Android rules, NDK configuration, native facade targets, and Android source
  directories are not present.

## Summary

Starting from the Bazel-only baseline, reorganize FastECU first without
changing behavior, then progressively establish this dependency direction:

```text
Desktop UI ─────┐
Desktop platform ├──► backend ───► algorithms
Android/JNI ────┘
```

Final source layout:

- `apps/desktop`: executable entry point and dependency composition.
- `src/ui/desktop`: Qt widgets, dialogs, forms, resources, and embedded hex editor.
- `src/platform/desktop/{common,unix,windows}`: Qt, filesystem, serial, J2534, remote, OpenSSL, and threading adapters.
- `src/backend`: portable application workflows and injected platform ports.
- `src/algorithms`: portable deterministic C++20 algorithms and models.
- `src/platform/android/native`: versioned native facade and Android build fixture.
- `resources/shared`: configurations and kernels.
- `resources/desktop`: fonts, icons, and desktop images.

Both `algorithms` and `backend` become Qt-, JNI-, and OS-independent. The future Kotlin application supplies a different UI and Android USB platform implementation while reusing both layers.

## Modularization Roadmap

1. **Capture the post-migration modularization baseline — complete (2026-07-19)**
   - Used the existing Bazel CI and packaging paths without adding a second build graph or restoring qmake compatibility.
   - Recorded the Windows, macOS, and Linux build/test matrix, Windows/macOS packaging checks, unchanged coverage baseline, and representative golden outputs from exact revision `3ce7c3d` before the first source move.
   - Added characterization vectors for parsers, expressions, checksums, protocol frames, ROM transformations, and existing flash planning before relocating code.

2. **Perform mechanical source organization - complete (2026-07-19)**
   - Move files in bounded groups using `git mv`; change only paths, includes, QRC references, and Bazel source lists.
   - Separate each flash-module dialog into desktop UI ownership and each `_operation` implementation into transitional backend ownership.
   - Move protocol codecs/math to algorithms, protocol drivers/orchestration to backend, and concrete transports to platform.
   - Preserve class names, behavior, `//:fastecu`, packaging entry points, and existing test labels.
   - Retain the aggregate Bazel target until every move passes the complete desktop matrix.

3. **Replace the monolithic Bazel graph — complete (2026-07-20)**
   - Added package-owned `BUILD.bazel` targets under each module and replaced `bazel/fastecu_sources.bzl` (deleted).
   - Kept `//:fastecu` as an alias to `//apps/desktop:fastecu`.
   - Made targets private by default (Task 8) and permit only:
     - `algorithms → algorithms`
     - `backend → algorithms/backend`
     - `platform → backend/algorithms`
     - `ui → backend`
     - `ui → algorithms` **(deviation from this bullet's original list, discovered and adjudicated during Task 8)**
     - composition roots → UI and platform implementations
   - **Deviation:** the permitted-directions list above gains `ui → algorithms`, which this bullet originally omitted. Seven real edges exist from `src/ui/**` directly into `src/algorithms/**` headers (`cipher.h`, `menu_command.h`, `qt_bytes.h`, `mut_dma_memory.h`); all seven run downward (ui depending on algorithms, never the reverse) and create no cycle. The Task 8 visibility lockdown grants this direction in every `src/algorithms/*/BUILD.bazel` package template.
   - **Also grandfathered (human decision, Task 5, unchanged by Task 8):** three `ui → platform` edges — `//src/ui/desktop` depends directly on `//src/platform/desktop/common/{logging,remote_utility,transport}` (`mainwindow.h` includes `systemlogger.h`, `remote_utility.h`, and three `fastecu_*_transport.h` headers). These are called out with `GRANDFATHERED` comments on the three platform packages' `default_visibility`, not covered by the general platform template.
   - Added a CI layering check for the one deliberately frozen violation: `//:serial_compat_allowlist` (a `py_test` running `scripts/check-serial-compat-allowlist.py`) fails the build if the `serial_qt_compat` transitional target's visibility allowlist grows past its frozen 20 entries (19 layering-violation debt entries carrying `serial_port_actions.h` into backend/ui callers, plus one legitimate same-layer `platform → platform` sibling edge to `remote_utility`). The list may only shrink as steps 5/6's backend and ui migrations remove callers.
   - Everything under `src/` and `apps/` is now visibility-restricted per the permitted directions above (previously all `//visibility:public`); a deliberately illegal `algorithms → ui` dependency was proven to be rejected by Bazel during verification.
   - Note for step 4's benefit: the broader Qt/JNI/OpenSSL closure check (rejecting those deps from the final algorithms/backend closures) was deliberately deferred, not implemented in Task 8. All 43 Qt-coupled files under `src/algorithms` would fail such a check today (`QT_DEPS` and `@openssl` are standing, adjudicated exceptions — third-party portability debt deferred on purpose). That check lands with step 4's first portable module instead.
   - Used explicitly named `*_qt_compat` targets during migration (`serial_qt_compat`); still transitional as of this date, to be removed once steps 5/6 finish removing its debt callers.

4. **Make all algorithms portable — complete (2026-07-22)**
   - Use the declared GoogleTest/GoogleMock 1.17.0.bcr.2 dependency for new
     Qt-free `cc_test` targets.
   - Introduce `fastecu`-namespaced byte containers, validated value models, typed identifiers, `Result<T>`, structured errors, and immutable parsed definitions.
   - Migrate in this order, writing golden tests before each conversion:
     1. Byte, endian, NRC/DTC, and expression helpers.
     2. MUT/DMA, SSM, CDBG, and flash-protocol codecs.
     3. Shared SSM framing, seed/key, payload cipher, CRC, and response validation.
     4. Every checksum family.
     5. Config, logger, RomRaider, and EcuFlash definition parsing.
     6. Calibration scaling/map calculations and ROM mutation.
     7. Flash block planning and verification algorithms.
   - Replace `QString`, `QByteArray`, `QVector`, and parallel `QStringList` models with standard C++ types.
   - Retain separate protocol-family state machines; do not create a universal flashing abstraction.
   - Every `src/algorithms` package is now split into a portable target plus a sibling `:qt_compat` shim, and `scripts/check-portable-closure.py` (wired into CI as `//:portable_closure`) confirms `OK: 9 portable algorithms targets, none reach Qt.` — proven non-vacuous by injecting a `QT_DEPS` dependency into a portable target and observing the check fail before restoring the file.
   - **Amendment 1:** step 4 narrowed to Qt removal only. `Result<T>`, structured errors, typed identifiers, validated value models, and the parsing/calibration/flash-planning migrations (originally items 5-7 of the ordered list above) move to step 5.
   - **Amendment 2:** the closure check rejects **Qt and JNI only**. `@openssl` remains in `src/algorithms/crypto`; **step 7 decides its fate** once NDK cross-compilation makes the real constraint visible.
   - **Amendment 3:** new tests are co-located as `src/**/*_test.cpp`, not added under `tests/`. Existing `tests/` files are retained as legacy contract holders against the `:qt_compat` shims and are retired only when those shims die.
   - **Amendment 4:** the checksum correction dialog is now **one aggregated summary** instead of one per family — a deliberate behavior change, bench-checklist item recorded in Task 9 Step 8.
   - Note for step 5's benefit: each `:qt_compat` target is transitional debt whose only remaining callers are backend and UI. Step 5 should drain them and delete the shims.

5. **Make backend workflows portable**
   - Define capability-specific ports for byte-stream/K-Line, CAN frames, SSM, file repositories, settings, monotonic clock/delay, cancellation, and event delivery.
   - Backend owns no threads. Platform code runs blocking, bounded, cancellable backend calls on Qt workers or future Kotlin coroutines.
   - Split `FileActions` and `MainWindow` responsibilities into definition, calibration, checksum, logging, and flash use cases.
   - Convert logging to typed `start`, bounded `poll`, and `stop` sessions. Samples carry stable channel ID, numeric/raw value, and unit; UI owns locale formatting.
   - Convert flashing to preflight plus execution: build and validate a `FlashPlan`, obtain UI confirmation before irreversible I/O, and execute without backend dialogs.
   - Migrate each ECU/TCU/EEPROM/JTAG/BDM operation family independently behind scripted transports, preserving its existing wire sequence.
   - Remove direct `QMessageBox`, `QFileDialog`, widget, filesystem, and `SerialPortActions` access from backend code.

6. **Finish the thin desktop shell**
   - Implement Qt adapters for backend ports and marshal events to the GUI thread.
   - Keep `MainWindow` and dialogs responsible only for presentation, input collection, signal wiring, and calling backend use cases.
   - Move construction and platform selection into `apps/desktop`.
   - Remove compatibility wrappers, obsolete facades, duplicate status macros, and the temporary aggregate implementation target.
   - Re-run packaging and the existing hardware bench checklists for affected logging/flashing paths.

7. **Add the Android-ready seam**
   - Pin `rules_android` 0.7.3, `rules_kotlin` 2.4.0, and Android NDK r29 (`29.0.14206865`) in Bazel configuration.
   - Define `--config=android_arm64_api29` for API 29 and `arm64-v8a`.
   - Add a versioned C ABI with `fastecu_v1_*` symbols, opaque session handles, fixed-width POD types, caller-owned buffers, structured error codes, and transport callback tables.
   - Expose MUT/DMA session lifecycle through create/start/poll/stop/destroy operations. Neither portable layer includes `jni.h`.
   - Add host ABI contract tests and a no-UI `//src/platform/android:android_native_smoke_apk` fixture proving Android cross-compilation, exported symbols, and native packaging.
   - Stop before adding the Kotlin product app, Android USB implementation, or real-device behavior.

## Public Interfaces and Error Handling

- Portable errors distinguish invalid configuration/definition, timeout, disconnected transport, malformed or rejected ECU response, cancellation, unsupported operation, and internal failure.
- Exceptions never cross backend ports or the native ABI.
- Reads must have explicit timeouts and support cancellation; platform teardown must unblock active reads.
- The later JNI adapter maps Kotlin data classes and USB operations to the versioned C ABI. STL and C++ object layouts are never exposed across JNI.

## Test and Completion Gates

- Run `bazel build -k --config=release //:fastecu //tests/...` and `bazel test -k --config=release //tests/... //:bazel_openssl_wiring` after every migration group.
- Require unchanged golden vectors for all mechanically moved or extracted behavior.
- Test parser validation, checksum correction/error outcomes, protocol malformed frames, timeouts, disconnects, cancellation, flash preflight rejection, and scripted successful operations.
- Require the portable algorithms/backend tests to run as Qt-free host `cc_test`s.
- Require Windows/macOS/Linux desktop CI, Windows/macOS packaging, coverage ratchet, module-boundary checks, and the Android arm64 smoke build.
- Keep real ECU/adapter verification as a separate documented bench gate; no reorganized flashing or logging path is considered hardware-qualified solely from unit tests.

## Assumptions

- Bazel is the sole supported build system. The qmake removal in `66dd62e` and ADR 0001 is complete, and this plan must not recreate qmake project files or source-list synchronization checks.
- All algorithms and shared backend workflows must be portable before Kotlin product development starts.
- Android v1 will target MUT/DMA live logging over USB serial, API 29, and `arm64-v8a`.
- Windows, macOS, and Linux desktop behavior remains supported throughout.
- Keep `rules_android` isolated behind the Android fixture so desktop targets do not depend on the Android rule stack. Tooling references: [rules_android](https://github.com/bazelbuild/rules_android), [rules_kotlin](https://github.com/bazel-contrib/rules_kotlin), [Android NDK](https://developer.android.com/ndk/downloads).
