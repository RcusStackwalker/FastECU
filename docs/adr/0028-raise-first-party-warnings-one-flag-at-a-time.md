# ADR 0028: Raise First-Party Warnings One Flag at a Time

## Status

Accepted. Implemented incrementally by `bazel/warnings.bazelrc`; the table
under Decision tracks which flags have landed.
[Issue #470](https://github.com/RcusStackwalker/FastECU/issues/470) tracks the
rollout.

## Context

[ADR 0018](0018-treat-first-party-warnings-as-errors.md) makes every warning in
first-party code fatal but leaves the levels at `-Wall` on clang and GCC and
MSVC's default `/W1`. A stricter level finds conversion, shadowing and
signedness bugs in byte-protocol code, but enabling it all at once means
hundreds of fixes in one change, many of them in hardware-facing code that must
not change behaviour.

A measurement of the candidate sets on all three CI toolchains (GCC 15, Apple
clang, MSVC 14.51, run 40096240827 in the issue) found 839, 211 and 165 unique
sites. Most of the GCC count is one flag: `-Wmissing-field-initializers` accounts
for 563 sites that clang does not report. Counts differ by compiler for the
same code, so a flag has to be judged on all three before it lands.

The same measurement looked for false positives. The flow-based GCC families
(`-Warray-bounds`, `-Wstringop-*`, `-Wmaybe-uninitialized`, `-Wrestrict`,
`-Wdangling-pointer`, `-Wuse-after-free`) reported nothing. `-Wnull-dereference`
reported 8 sites, all GCC-only and all false: two on a `std::vector` the code
just sized, two on a `back()` of a non-empty constant, and four inside libstdc++
(`std::expected`'s constructor and `std::swap`) that our code cannot edit.

## Decision

- Strictness is raised **one flag per change**, applied to all first-party
  code at once. Every site is fixed on all compilers in the same change, and
  the flag's line in `bazel/warnings.bazelrc` is added last. There is no list
  of exempt directories, so new code is held to every flag that has landed.
- A clang/GCC flag lands together with the closest MSVC warning. A flag with no
  MSVC counterpart (`-Wold-style-cast`, `-Wextra-semi`) is left out of the MSVC
  line. `/W4 /permissive-` is the first change because it has no clang/GCC
  counterpart.
- The flags live in `bazel/warnings.bazelrc`, imported by `.bazelrc`: one
  `--per_file_copt` line per compiler family, matched against first-party paths
  and the `bazel-out/<cfg>/bin/` prefix that generated moc/uic output carries.
  Dependencies are untouched.
- `--features=external_include_paths` is enabled for every build, so Qt,
  googletest and pugixml headers are `-isystem` (`/external:I` with
  `/external:W0` on MSVC) and only our own code can warn.
- Generated moc/uic output is first-party. If a flag fires in Qt-generated code
  we cannot edit, that flag gets a narrow exclusion for generated files, written
  next to the flag and named here.
- **False positives are not suppressed in source.** ADR 0018's rule stands: no
  `#pragma`, no `-Wno-*`. A diagnostic is first fixed at its cause, which for
  conversions means an explicit `static_cast` where the value is known to fit.
  A flag that still produces a false positive no rewrite can remove is dropped
  or deferred, with the evidence recorded here.
- A fix never changes behaviour. A real bug found while cleaning up becomes its
  own issue and a test-first change.
- A flag is dropped by `-Wno-<flag>` after the flag that includes it in the
  `bazel/warnings.bazelrc` line, never by a pragma. The only drop so far is
  `-Wmissing-field-initializers` (inside `-Wextra`): GCC reports 563 sites where
  a designated initializer omits a member without a default member initializer,
  and clang does not. Silencing it needs `{}` on every such member, which
  clang-tidy's `readability-redundant-member-init` rejects for 183 of the 239
  members involved (class types such as `std::string`), so the two checks
  cannot both be satisfied. Listing every member at the call sites would make
  idiomatic designated initializers worse.
- CI reports no warning counts. The warnings-as-errors gate is the contract.

| Flag | Status |
| --- | --- |
| MSVC `/W4 /permissive-`, `external_include_paths` | landed |
| `-Wextra-semi`, `-Wcast-qual`, `-Wunused-parameter` | landed (`-Wunused-parameter` pairs with C4100, already in `/W4`) |
| `-Wextra` | landed, without `-Wmissing-field-initializers` |
| `-Wmissing-field-initializers` | dropped: GCC-only, contradicts `readability-redundant-member-init` |
| `-Wshadow` | landed |
| `-Wnon-virtual-dtor`, `-Woverloaded-virtual`, `-Wformat=2`, `-Wimplicit-fallthrough`; MSVC C4263, C4265, C4296, C4826, C5262, C4062 | landed with `-Wshadow`: no sites on any compiler |
| `-Wold-style-cast` | planned |
| `-Wconversion`, `-Wsign-conversion` | planned |
| `-Wnull-dereference` | dropped: GCC-only false positives, half inside libstdc++ |

## Alternatives

- An exempt-directory list that shrinks area by area: new code is strict from
  day one, but the list must be repeated per compiler family and kept
  identical, and a strict translation unit that includes an exempt header fails
  on that header, so the list has to be measured by translation unit rather
  than by directory. The codebase is small enough to take a flag at a time.
- Narrow `#pragma` suppressions for compiler false positives: three of the four
  measured clusters are inside standard-library headers, where a pragma at the
  call site is the only option and hides a diagnostic nobody can act on. It
  would also reverse ADR 0018.
- Per-target `copts` through wrapper macros: rejected in ADR 0018 for the same
  reason; a target written without the macro silently escapes.

## Consequences

- Each change is reviewable on its own, and a compiler upgrade that adds a
  false positive to an enabled flag is answered by dropping or deferring that
  flag in this ADR, not by a suppression.
- `-Wmissing-field-initializers` and `-Wnull-dereference` are not enforced. Real null dereferences are caught by
  clang-tidy and the sanitizer builds.
- The Android NDK build is still not gated by warnings-as-errors (ADR 0018), so
  its warnings are not enforced; gating it is separate work.
- `bazel/fuzz.bazelrc` still enables `external_include_paths` for its own
  configs; it is redundant with the repo-wide setting and can go when that file
  is next touched.
