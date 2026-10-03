# Zero Warnings and Warnings-as-Errors Design

## Intent and scope

Resolve [issue #48](https://github.com/RcusStackwalker/FastECU/issues/48):
bring first-party code to zero compiler and linker warnings at the warning
levels the build enables today, then make every warning fatal so the count
stays at zero.

"First-party" means every compile and link action owned by the main
repository: our sources and the moc/uic output generated from them. "Today's
levels" means the toolchain defaults plus `COMMON_COPTS` — `-Wall` and
Bazel's small additions on clang and GCC, and the MSVC default (no `/W`
flag, effectively `/W1`). The gate covers every toolchain CI builds with:
Apple clang (macOS job), GCC 15 (Linux job), clang on Linux (coverage and
SonarCloud), the Android NDK clang (portable-core job), and MSVC for both
the x64 build and the x86 J2534 bridge toolchain.

Out of scope:

- Raising warning levels (`-Wextra`, `-Wconversion`, `-Wold-style-cast`,
  MSVC `/W4`, and the rest of a strict set). That is a follow-up issue; the
  measurements taken for it are summarized at the end of this document.
- Bazel's own `WARNING:` lines and tool notices (`llvm-profdata`,
  install-qt-action), which are not compiler or linker diagnostics.
- Warnings raised while compiling external repositories' own sources.
- clang-tidy and SonarCloud findings, which have their own gates.

## Current state

Measured on master `5a0420c5` with a clean, uncached local macOS build and
the CI logs of run 36923550020, the last full-compile run before the
BuildBuddy remote cache landed. Cached actions do not replay their
diagnostics, so only uncached builds give a complete inventory.

| Toolchain | Compiler warnings | Linker warnings |
| --- | --- | --- |
| Apple clang | 15 sites: 13 unused and set-but-unused locals, one deprecated Qt signal, one libc++ `#warning` | 520 × duplicate `-rpath` |
| GCC 15 | about 29 direct sites plus about 22 `EXPECT_EQ` instantiations: sign-compare, unused `write()` results, range-for copies, unused locals, `-Warray-bounds` false positives | none |
| clang on Linux (coverage) | the Apple clang set; nothing new | none |
| MSVC | 4 × C4834 discarded `[[nodiscard]]`, 3 × D9025 command-line override | 1 × LNK4070 |
| Android NDK clang | none | none |

No suppression flags exist except `-Wno-implicit-function-declaration`,
which three Bazel files pass on macOS. It names a C-only diagnostic and does
nothing for C++ translation units.

## Fixes

Each warning is removed at its cause. Nothing is suppressed, and no source
gains a `#pragma`. Sites are named by file and symbol; line numbers in the
CI inventory have already drifted, so the implementation re-measures first.

**A. Unused and set-but-unused locals.** `J2534_unix.cpp` (`rxmsg`,
`numRxMsg`, `timeout`, `fw_version`, `cfgitem`),
`serial_port_actions_direct.cpp` (`msglength`, `inbuf`), `mainwindow.cpp`
(`hierarchyLevel`, `map_index`, two `obj`), `definition_file_convert.cpp`
(`line_index`), and `tst_mut_dma_integration.cpp` (two `mock`). Delete the
variable. If its initializer has a side effect, keep the expression as a
statement. In the J2534 and serial sources the change is the deletion and
nothing else.

**B. Deprecated Qt signal.** `calibration_maps.cpp` connects
`QCheckBox::stateChanged(int)`; move it to `checkStateChanged(Qt::CheckState)`
with the slot's behavior unchanged.

**C. Signed/unsigned comparisons (GCC `-Wsign-compare`).**
`ssm_protocol_core.cpp`, `biu_operations_subaru.cpp` (two sites),
`scripted_ssm_transport.h` (two sites), and about 22 test assertions that
compare an `int` literal or index with a `size()`. Make the operand types
agree — an unsigned literal or a `std::size_t` index. Use `std::cmp_*` only
where both operands can genuinely carry either sign. No cast may change a
value.

**D. Ignored `write()` results (GCC `-Wunused-result`).**
`direct_backend_pty_test.cpp` (four sites), `mock_openport.h`,
`serial_pty_e2e_test.cpp`, and `tst_mut_dma_integration.cpp` (two sites).
glibc's `warn_unused_result` is not silenced by a `(void)` cast. These
writes feed test fixtures, so check the byte count: tests assert it, and the
mock handles a short write.

**E. Range-for copies (GCC `-Wrange-loop-construct`).** `dispatch_test.cpp`
and the two SH7058 CAN executor tests bind structured bindings by value;
bind them by `const auto&`.

**F. GCC 15 `-Warray-bounds` false positives.** Six call sites build a
padded image with `bytes::Bytes rom(n, 0xFF)` and then
`rom.insert(rom.end(), …)`: the MH8104 and MH8111 CAN executors and four
tests (the two SH7058 CAN executor tests, the Hitachi M32R K-Line executor
test, and the Unisia JECS M32R boot-mode plan test). GCC 15.3 reproduces the
warning on the MH8104 executor, and three rewrites compile clean:
`append_range`, reserve-then-insert, and size-then-copy. Use
`rom.append_range(…)`; Apple clang 21's libc++ supports it. If the Android
NDK's libc++ does not, use size-then-copy instead. Before rewriting either
executor, confirm a test asserts the padded read result, and add a
characterization test if none does.

**G. Discarded `[[nodiscard]]` results (MSVC C4834).** MSVC's
`std::expected` is `[[nodiscard]]`; libc++'s and libstdc++'s are not, which
is why only MSVC reports these. `provisioning.cpp` discards
`copy_file` on purpose — the comment there says a missing previous config
is not an error — and becomes `std::ignore = fs.copy_file(…);` with the
comment kept. The three test sites (`provisioning_test.cpp` twice,
`in_memory_file_system_test.cpp`) assert the result with
`ASSERT_THAT(…, IsOk())` instead of discarding it.

**H. Stale flags that produce warnings.** `tests/force_asserts` passes
`-mmacosx-version-min=10.15`, which libc++ answers with a `#warning`; the
global `--macos_minimum_os` already applies, so drop it. Three J2534 bridge
tests pass `/UNDEBUG`, which MSVC reports as D9025 because release adds
`/DNDEBUG`; after the GoogleTest migration none of them calls `assert()`, so
drop the flag and the stale comment that describes `assert()` as their test
protocol.

**I. Linker warnings.** `rules_qt`'s macOS BUILD gives every Qt module the
same `-rpath` linkopt, so each link repeats it once per module. Add
`bazel/patches/rules_qt_mac_single_rpath.patch`: move the rpath into one
shared `cc_library` that every module depends on, so each link carries it
once. For LNK4070, delete the `LIBRARY` line from `tests/fake_j2534_dll.def`
so the linker takes the name Bazel gives the output.

**J. Dead suppression.** Remove `-Wno-implicit-function-declaration` from
`bazel/qt_common.bzl`, `bazel/qt_remote_objects.bzl`, and
`tests/force_asserts/BUILD.bazel`.

**K. One idiom for intentional discards.** The tree discards about 38
results with `static_cast<void>(call)` or `(void)call` and has no
`std::ignore`. Convert them all to `std::ignore = call;` (from `<tuple>`)
and add the rule to the coding style guide: an intentionally discarded result is assigned to
`std::ignore`, and a comment says why when the reason is not obvious.
`(void)name;` for an unused parameter or variable is a different idiom and
stays.

## Enforcement

A new `REPO.bazel` enables the toolchain's own feature for the main
repository:

```starlark
repo(features = ["treat_warnings_as_errors"])
```

The feature makes compile warnings fatal (`-Werror`, `/WX`) and link
warnings fatal too (`-Wl,-fatal_warnings` on macOS, `-Wl,-fatal-warnings` on
Linux, `/WX` on the MSVC linker). A scratch workspace confirmed the
semantics this design relies on:

- it applies to every package in the main repository, including packages
  that call `package(…)` themselves;
- it does not reach external repositories, whose own compiles stay as they
  are;
- a negative feature wins, so `--features=-treat_warnings_as_errors` on the
  command line disables it.

It is always on, locally and in CI, with no `--config`. Local builds and CI
then fail on the same things, and BuildBuddy cache keys stay shared. The
command-line negative feature is a documented local escape hatch for
toolchain drift, such as a new Xcode adding a diagnostic; it never goes into
committed configuration. No target opts out, and reviewers reject a
committed `-treat_warnings_as_errors`.

Two toolchains need attention:

- `bazel/toolchains/windows_x86_msvc` is a rule-based toolchain that defines
  its features explicitly, and an undefined feature is silently ignored. Add
  a `treat_warnings_as_errors` feature that passes `/WX` to compile and link
  actions, known but not enabled by default, so the J2534 bridge binaries
  are gated too.
- Check whether `rules_android_ndk`'s toolchain defines the feature. If it
  does not, record that in the ADR rather than adding a toolchain. The
  portable code it builds is already gated by the three desktop builds.

clang-tidy reads compile commands extracted from the build, which will now
carry `-Werror`. It reports compiler errors whatever its check filter says,
and it runs a newer LLVM than the build compiler, so a diagnostic new to
that LLVM would fail the clang-tidy gate. `scripts/clang_tidy_runner.py`
adds `-Wno-error` to clang-tidy's compiler arguments (or `/WX-`, if the
Windows clang-cl driver needs that spelling), and the compiler build stays
the only warnings gate.

## Documentation

- ADR 0018, "First-party warnings are errors": the scope, why `REPO.bazel`
  rather than `--copt=-Werror` (which reaches external repositories) or a
  CI-only config (local drift and a split cache), and the accepted cost that
  a compiler upgrade can break the build and is fixed forward.
- [CLAUDE.md](../../../CLAUDE.md): one rule under build-graph guardrails —
  warnings are errors in first-party code; fix the cause; never commit an
  opt-out; the command-line negative feature is a local escape hatch only.
- [Coding style guide](../../coding-style.md): the `std::ignore` rule from
  fix K.

## Rollout

Two pull requests as a `gh stack`:

1. `fix: clear compiler and linker warnings` — fixes A to K, with the
   `std::ignore` rule in the coding style guide.
2. `build: treat first-party warnings as errors` — `REPO.bazel`, the x86
   toolchain feature, the clang-tidy change, ADR 0018, and the CLAUDE.md
   rule. Its CI run is the proof of zero: any warning left on any platform
   fails as an error there. Fixes found that way go back into the first PR.

## Compatibility and failure modes

The production edits are deletions of unused locals, type-matching in
comparisons, the Qt signal change, the `append_range` rewrite in two
executors, and the `std::ignore` conversions. None changes behavior; existing
tests cover them, and fix F adds a characterization test where the padded
read is not already asserted. The J2534 and serial edits are deletions only,
so no bench re-qualification is needed, and the first PR says so.

With the feature on, a compiler upgrade on a CI image or a developer machine
can fail the build on a new diagnostic. The response is to fix it; the
escape hatch exists only so local work is not blocked meanwhile.

## Verification and exit criteria

- Before the second PR: a clean, uncached macOS build of release `//...`
  prints no compiler or linker warning. Linux and Windows are verified by
  the second PR's CI.
- The second PR temporarily carries a commit adding an unused local to a
  portable source. The macOS, Linux, Windows, and portable-core jobs must
  all fail on it; then the commit is dropped. Once the feature is on, a
  warning is a failed action and is never cached, so caching cannot hide
  one.
- All CI jobs pass on the second PR: Bazel on three platforms, portable
  core, SonarCloud, clang-tidy, and pre-commit.
- `bazel test --config=release //...` passes on each platform.
- Issue #48 closes when the second PR merges.

## Follow-up: raising the warning level

Tracked in [issue #470](https://github.com/RcusStackwalker/FastECU/issues/470),
not part of this work. A strict clang set
(`-Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wold-style-cast` and related flags) applied to first-party files on macOS
reports 543 sites in our sources: 176 `-Wold-style-cast`, 170
`-Wsign-conversion`, 118 other conversions, and about 80 for the `-Wextra`
core. Most sit in
`J2534_unix.cpp`, `biu_operations_subaru.cpp`,
`serial_port_actions_direct.cpp`, and the vendored hex editor; the portable
core is nearly clean. Another 200 come from Qt headers reached through `-I`
virtual includes, which Bazel's `external_include_paths` feature turns into
`-isystem`. `--per_file_copt` path regexes can apply strict flags to
first-party sources and generated moc output while excluding `external/`.
GCC and MSVC `/W4` were not measured.
