# ADR 0018: First-Party Warnings Are Errors

## Status

Accepted and implemented by `REPO.bazel`.

## Context

Compiler and linker warnings accumulated unnoticed
([issue #48](https://github.com/RcusStackwalker/FastECU/issues/48)): about
fifty distinct sites across Apple clang, GCC, and MSVC, plus 520 duplicate
`-rpath` linker warnings on every macOS build. A warning only prints when its
action runs, and Bazel's local and remote caches replay outputs without their
diagnostics, so an incremental or cached build hides every warning it already
produced. Counting warnings is unreliable; failing on them is not.

## Decision

`REPO.bazel` requests the C++ toolchain's own `treat_warnings_as_errors`
feature for every package in the main repository:

```starlark
repo(features = ["treat_warnings_as_errors"])
```

- Compile and link warnings both fail: `-Werror` and `-Wl,-fatal_warnings`
  (macOS) or `-Wl,-fatal-warnings` (Linux) on clang and GCC, `/WX` for the
  MSVC compiler and linker.
- Only the main repository is affected. External repositories (googletest,
  pugixml, Qt) compile as they always have; warnings their headers raise in
  our code are ours.
- It is always on, locally and in CI, with no `--config`, so local builds and
  CI fail on the same things and share cache keys.
- No target opts out. A negative feature wins, so
  `--features=-treat_warnings_as_errors` on the command line turns it off for
  one local build, for example after a toolchain upgrade adds a diagnostic.
  It never goes into committed configuration.
- The auto-configured clang, GCC, and MSVC toolchains define the feature.
  The hand-written x86 MSVC toolchain in
  `bazel/toolchains/windows_x86_msvc` defines its own, because a rule-based
  toolchain silently ignores a feature it does not know. `rules_android_ndk`
  0.1.5 does not define it, so the Android cross-compile is not gated; the
  desktop builds compile the same portable code.
- clang-tidy runs with `-Wno-error`: it analyzes compile commands that carry
  `-Werror`, reports compiler errors regardless of its check filter, and uses
  a newer LLVM than the build. The build is the warnings gate.
- Warning levels are unchanged: `-Wall` on clang and GCC, MSVC's default.
  Raising them is [issue #470](https://github.com/RcusStackwalker/FastECU/issues/470).

## Alternatives

- `--copt=-Werror` in `.bazelrc`: reaches external repositories, which this
  project does not control, and needs a different spelling per compiler.
- A CI-only `--config=werror`: local builds stop failing on what CI fails
  on, and the two get different action keys, so they stop sharing cache
  entries.
- Per-target `copts` through wrapper macros: every plain `cc_library` would
  move to a macro through Gazelle, and any target written without it would
  silently escape.

## Consequences

- A compiler upgrade on a CI image or a developer machine can fail the build
  on a new diagnostic. The fix goes forward; the command-line negative
  feature only unblocks local work meanwhile.
- A warning is a failed action and is never cached, so a cached build can no
  longer hide one.
- Warnings are fixed at their cause. Source-level `#pragma` suppressions,
  `-Wno-*` flags, and `-Wno-error=` are not used; an intentionally discarded
  result is written `std::ignore = call;` (see the
  [coding style guide](../coding-style.md#error-handling)).
- One pre-existing flag stays: `-Wno-implicit-function-declaration` in
  `COMMON_COPTS` on macOS. Qt 6.8.3's `qyieldcpu.h` calls the compiler
  intrinsic `__yield()` without declaring it, and Apple clang rejects that
  unless the diagnostic is off. It works around a third-party header; it
  adds no first-party exception.
