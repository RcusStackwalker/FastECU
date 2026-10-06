# FastECU Modularization and Android-Readiness Plan

This roadmap owns the remaining Android seam. Current architecture and rationale
are in the [design notes](design-notes.md); broader cleanup and unresolved defects
are in [technical debt](tech-debt.md). Completed milestone records are recoverable
through [Git history](README.md#recover-completed-work-and-deleted-source-citations).

## Remaining roadmap

### 7 — Android seam

The portable-core cross-compilation spike is implemented: backend and algorithms,
including portable GoogleTest binaries for compile/link only, build for Android
arm64 with C++23. The native facade and smoke fixture have not started. Android
v1 targets MUT/DMA live logging over USB serial, API 29, `arm64-v8a`; the Kotlin
product application and real Android USB implementation are later work.

The spike uses `rules_android_ndk` 0.1.5 and was verified against NDK r30
(30.0.16248370). [Android configuration](../bazel/android.bazelrc) is opt-in through
`--config=android`; desktop builds need no NDK. The
[portable-core gate](../scripts/android-cross-compile.sh) requires `ANDROID_NDK_HOME`,
builds everything under backend/algorithms, and rejects reachable platform
labels. CI runs it separately from desktop `bazel build //...`.

Remaining milestones:

1. **Pin deployment/toolchain inputs.** Set API 29 explicitly (the spike used the
   toolchain default) and establish a hermetic NDK pin. Keep Android dependencies
   isolated; add Kotlin rules only if the fixture needs them.
2. **Add the native facade under `src/platform/android/native`.** Export versioned
   `fastecu_v1_*` C symbols with opaque handles, fixed-width POD values, caller-owned
   buffers, structured errors, and transport callbacks. Wrap the existing logging
   use case and cancellation/event contracts; callers supply execution context.
   Keep STL layouts and exceptions behind the ABI and JNI out of the portable core.
3. **Verify the ABI and smoke fixture.** Host tests cover ownership, buffer sizing,
   invalid inputs, cancellation, errors, and teardown. An API-29/arm64 no-UI
   `android_native_smoke_apk` proves native compilation, exported symbols, and
   packaging. Desktop builds remain independent of Android setup.

These exit gates establish a native seam, not Android USB behavior or device
qualification. Generic Sonar cleanup, UI policy migration, and evidence-dependent
protocol corrections are separate debt, not hidden Android prerequisites.

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
