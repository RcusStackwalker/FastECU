# Native clang-tidy qualification

This dated record covers the LLVM 23.1.2 native-check migration and assertion
rollout. It records software analysis evidence; it does not qualify flashing
hardware. The [coding guide](../coding-style.md#assertions) owns conventions and
[tooling](../tooling.md#repository-checks) owns the supported execution workflow.

## Scope and correctness

The macOS ARM64 release compilation database contains 546 selected first-party
translation units. Native checks passed that entire filtered scope with zero
findings and successful exact exception-usage auditing after the final review
fixes and lambda-reachability cleanup in `bdd0e9c4`. Included first-party `.h`
and `.hpp` headers share the common filter; generated/vendor headers are excluded. Sources
selected out of this build configuration require their matching platform or
configuration gate.

All 42 real-dependency native fixtures pass locally after the independent
review fixes. Their regressions cover conditional/repeated single-use
execution, condition mutation order, continue back edges, independent lambda
analysis, and trace-only nested loops. Each failed before its fix. The
original 14 migration fixtures also pass with both the YAML implementation and
the delivered native checks. Their 58 exported diagnostics match after normalizing the intentional
`custom-` prefix removal and temporary fixture directory: names, file offsets,
severities, messages, notes, and replacements are otherwise identical.

The full release test graph passes locally: 294 tests passed and 20
platform-incompatible targets were skipped. Full pre-commit gates passed;
focused runner/exception tests also pass after the Windows runner-discovery fix.
The combined built-in/native macOS report found four missing-brace findings in
changed helpers; all were fixed, their three translation units passed the full
check set again, and all three affected suites passed. All four suites affected
by the final lambda cleanup also passed. The analyzer/test-cleanup revision,
`bdd0e9c4`, passed the combined 1,092-unit Linux CI scope with zero findings and
all 42 fixtures in 64.357 seconds in
[run 39417827686](https://github.com/RcusStackwalker/FastECU/actions/runs/39417827686/job/118611205491).
All platform application builds, Android, sanitizers/fuzz seeds, Sonar,
pre-commit and Gazelle passed on that revision. The subsequent benchmark-only
fingerprint guard passed its regression and the full local release graph. It
does not change the qualified analyzer sources or artifacts.

The exact central policy has two matcher-contract exceptions and 13 immutable
Mitsu fixture-result exceptions. Fatal GoogleTest `SetUp` proves the latter
before a body runs, and GoogleTest skips bodies on fatal setup failure. These
are documented framework proofs, not a baseline of accepted violations. A
fixture proves that an adjacent test without the exact identity remains
subject to enforcement; full-scope auditing rejects unused entries.

## Platform packaging

The [packaging record](../../scripts/clang_tidy_plugin/qualification/README.md)
contains archive hashes and ABI observations. The exact official macOS and
Linux archives both built and passed the registered template diagnostic/fix-it
smoke. Ordinary application target analysis also succeeded with LLVM discovery
unavailable.

Windows uses a Bazel-built linked host because the pinned executable exports no
clang-tidy symbols. The archive's static LLVM libraries require static release
CRT settings and absent zlib/zstd libraries, supplied through Bazel. Generated
force-linker feature switches reflect the module libraries actually installed.
Clang-cl consumes LLVM dependencies as system headers through `/imsvc`; a probe
confirmed dependency warnings are excluded while identical first-party warnings
still fail `/WX`.

The Windows linked host built, linked, validated all 16 registered checks,
reported zero findings in its 41-unit exclusive scope, and passed all 42
real-dependency fixtures in 61.760 seconds in
[CI run 39417827686](https://github.com/RcusStackwalker/FastECU/actions/runs/39417827686/job/118611205642)
on the qualified analyzer revision, `bdd0e9c4`.
This includes the template diagnostic and fix-it. Application builds on macOS,
Linux, and Windows, the Android portable build, sanitizers/fuzz seeds, and Sonar
also passed on that revision.

## Runtime comparison

The comparison used one frozen 546-unit release compilation database, eight
jobs on an Apple M5 Pro host (15 logical CPUs, 24 GiB memory), LLVM 23.1.2,
profiling in every mode, and recorded source/configuration/
artifact hashes. Four warmups preceded five measured samples per mode in
alternating forward/reverse rounds. All 24 passes returned zero diagnostics;
every extended pass also audited all 15 exact exceptions. The offline native
comparison artifact contains only the original nine checks, removing assertion
extensions so their cost is not charged to migration.

| Mode | Median seconds | Sample range, seconds |
| --- | ---: | ---: |
| Original nine YAML checks | 90.00 | 87.04–133.19 |
| Original nine native checks, original header filter | 89.43 | 87.74–131.82 |
| Original nine native checks, wider header filter | 121.42 | 87.79–134.62 |
| Delivered 16 checks and assertion extensions | 160.36 | 108.14–162.63 |

The raw median ratios are −0.6% for migration, +35.8% for wider coverage and
+32.1% for the delivered extensions. The latter two exceed the 10% investigation
threshold. Investigation found approximately 1.5× variation within identical
modes; per-file timings showed a broad speed change across the same translation
units. Two additional balanced rounds preserved every original sample and
confirmed that variation. Three rounds had wider/native ratios within 1.8%;
two crossed the speed transition and produced much larger ratios. These data
do not reliably isolate the wall-time cost of header coverage.

Adjacent all-checks/wider comparisons add 19.1–33.9%. Profiles attribute
substantial additional work to loop traces, uniform bytes and single-use
results. The required checks remain enabled with this measured cost recorded;
no timing-sensitive CI gate is introduced. Profile seconds are summed over
files and are not elapsed wall time. These measurements isolate repository
checks, including parsing and artifact loading, with built-ins disabled equally
in every mode. The combined report is separate correctness evidence; no
end-to-end CI speedup is claimed.

[Raw measurements and inputs](clang-tidy-plugin-qualification/measurements.json)
include all samples, profiles, selected sources, hashes, mode configurations,
and the original-nine source patch and Bazel build definition.
[Original normalized diagnostics](clang-tidy-plugin-qualification/original-diagnostics.json)
retain all 58 fixture diagnostics shared by the YAML, delivered native and
original-nine comparison artifacts. A subsequent regression fix makes the
public benchmark fingerprint include the scope manifest; it failed before the
fix and passed afterward. That guard runs outside timed analyzer work and does
not change the measured analyzer artifacts.

## Preservation

The approved design and plan remain in this PR so their original contents reach
delivered history. Retire them in a separate change after that preservation;
adding and removing them inside one squash would discard the original records.

[Execution decisions and tradeoffs](clang-tidy-plugin-qualification/execution-decisions.md)
preserve all rulings and their costs in order. No independent review findings
remain deferred.
