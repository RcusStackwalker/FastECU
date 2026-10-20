# FastECU Modularization and Android-Readiness Plan

This roadmap owns the remaining Android seam. Current architecture and rationale
are in the [design notes](design-notes.md); broader cleanup and unresolved defects
are in [technical debt](tech-debt.md). Completed milestone records are recoverable
through [Git history](README.md#recover-completed-work-and-deleted-source-citations).

## Remaining roadmap

### 7 — Android seam

The portable-core cross-compilation spike is implemented: backend and algorithms,
including portable GoogleTest binaries for compile/link only, build for Android
arm64 with C++23. The Android application is the desktop Qt UI with full
parity, over a wired OpenPort 2.0 or a Bluetooth STN-class adapter, API 29,
`arm64-v8a`. It is the last phase of the
[size-reduction strategy](superpowers/specs/2026-10-18-size-reduction-strategy.md#6-android),
which owns its sequencing.

The spike uses `rules_android_ndk` 0.1.5 and was verified against NDK r30
(30.0.16248370). [Android configuration](../bazel/android.bazelrc) is opt-in through
`--config=android`; desktop builds need no NDK. The
[portable-core gate](../scripts/android-cross-compile.sh) requires `ANDROID_NDK_HOME`,
builds everything under backend/algorithms, and rejects reachable platform
labels. CI runs it separately from desktop `bazel build //...`.

The [NDK rules patch](../bazel/patches/rules_android_ndk_pic.patch), applied by
[the module graph](../MODULE.bazel), declares the `pic` feature implemented by
the toolchain's existing unconditional `-fPIC`. This lets `--force_pic` pass
feature validation without changing Android compiler flags. The
[build code-generation policy](tooling.md#build-code-generation) enables that
flag on Unix hosts while preserving the native Windows mode.

Remaining milestones:

1. **Pin deployment/toolchain inputs.** Set API 29 explicitly (the spike used the
   toolchain default) and establish a hermetic NDK pin. Keep Android dependencies
   isolated.
2. **Build the Qt application for Android.** Prove that Bazel and `rules_qt`
   package the existing Qt Widgets application as an API-29/arm64 APK. Desktop
   builds remain independent of Android setup.
3. **Add the Android platform layer under `src/platform/android`.** USB serial
   and BLE byte pipes for the portable OpenPort and STN drivers, and
   permissions. Use JNI only where Qt does not cover the platform API, and keep
   it out of the portable core.

The APK ships the native code and the UI together, so there is no versioned C
ABI or separate Kotlin application. These exit gates establish the Android
build and transports, not device qualification. SonarCloud cleanup, UI policy
migration, and evidence-dependent protocol corrections are separate debt, not
hidden Android prerequisites.

## Verification and qualification

Use the [repository verification commands](../AGENTS.md#build-and-verify) and
[coding/testing conventions](coding-style.md). Portable-core cross-compilation
is a separate gate; desktop Windows/macOS/Linux CI and release packaging still
apply to code changes. Preserve byte/outcome regressions and add ABI/smoke gates
when those milestones introduce them.

The [qualification matrix](flash-qualification-matrix.md) and its family checklists
own hardware status. Automated success never advances that status. Desktop
procedures are under [checklists](checklists/); read the relevant procedure for
logging, connection, diagnostics, platform selection, or checksum presentation.
