# Repository clang-tidy enforcement

Status: design and implementation plan approved on 2026-10-19. Native execution
started; platform packaging qualification precedes enforcement changes.

## Intent

Enforce the [assertion conventions](../../coding-style.md#assertions) locally
and in CI, and migrate the existing `.clang-tidy` `CustomChecks` into the
repository's compiled clang-tidy extension. The current integration is described
in [tooling](../../tooling.md#static-analysis).

## Settled decisions

- Account for every assertion convention in the design. Enable blocking
  diagnostics only for reliably detectable violations, and document detection
  limits explicitly.
- Fix existing violations before enabling each check as blocking. Roll out
  checks incrementally with a clean enforced scope, without a permanent
  violation baseline.
- New checks initially provide diagnostics. Consider fix-its separately where
  evaluation order, fatal-return behavior and imports can be preserved.
- Encode established exceptions, including ordinary `std::optional` checks and
  low-level Result API tests. Additional exceptions use narrowly scoped,
  documented central configuration rather than routine inline suppressions.
- Extend the existing extension. Establish local/CI integration and migrate
  all nine YAML checks with their existing diagnostics and permitted
  counterexamples before expanding assertion coverage. Preserve the existing
  template check's fix-it; diagnostics-only applies to new checks.
- Provide equivalent enforcement on macOS/Linux locally and Linux/Windows CI,
  including Windows-exclusive code. Investigate dynamic loading first; use a
  repository-built clang-tidy executable with linked checks if Windows requires
  it. The packaging choice remains subject to technical verification.
- Require the CI LLVM release for the supported local workflow. Build against
  that installation, validate compatibility before analysis, and provide one
  Bazel command to build and run the extension.
- Start with intraprocedural Result-state reasoning: recognize guards and
  fatal assertions within a function. Enforce plan-helper return conventions
  separately. Document unsupported cases and accept control flow that already
  proves the Result's state without demanding redundant assertions.
- Diagnose single-use locals only for adjacent declaration/assertion pairs
  where inlining preserves evaluation and destruction order. Other cases
  remain review responsibilities.
- Permit `Not(IsOk())` through central exceptions identifying the enclosing
  test/helper and explaining why its contract accepts any error. Otherwise
  require an explicit error matcher.
- Require a trace inside each assertion-bearing loop iteration. Detect direct
  assertions and assertions in lambdas invoked there initially; calls to
  separate assertion-bearing helpers are a documented detection limit.
  Parameterized tests still need traces for additional assertion loops.
- Keep separate parameterized cases for expensive transfer outcomes as a
  documented review requirement until the project defines expense precisely.
  Enforce mechanically detectable loop tracing and named parameterized cases.
- Changes to plugin sources, check configuration or exception configuration
  force full-scope analysis. Required positive and permitted-counterexample
  fixtures run in CI; missing extensions and skipped required fixtures fail
  the gate.
- Use the wider first-party header scope for both built-in and repository
  checks: `.h` and `.hpp` under `src/`, `apps/`, `tests/`, and `scripts/`,
  retaining generated/vendor exclusions. Clean newly exposed findings before
  enabling the unified scope. Analyze built-in and repository checks together
  and remove the separate custom-check pass and experimental adapter once the
  YAML migration is complete. Preserve scope selection, diagnostic reporting,
  profiling and full-scope invalidation in the simplified runner.
- Register checks consistently as `fastecu-*`, removing LLVM's automatic
  `custom-` prefix during migration. Update configuration, fixtures, runner
  and benchmarks together while preserving diagnostic behavior.
- Consume an explicitly selected, validated installation of CI's pinned LLVM
  release. Keep build/run orchestration in Bazel and document installation;
  automatic LLVM downloads are deferred until setup experience warrants them.
- Before removing a YAML implementation, require its fixtures to pass and
  compare normalized full-scope diagnostic names, locations, severities and
  messages. Normalize the intentional name-prefix change; review all other
  differences explicitly. Compare equivalent header scopes separately from
  cleanup exposed by widening built-in coverage.
- Benchmark on the same machine and scope with reproducible inputs and
  investigate runtime regressions above 10 percent. This is a review threshold,
  not a timing-sensitive CI gate. Measure new assertion checks separately.
- Track success and error states separately before Result access. Recognize
  terminating guards and invalidate known state after reassignment or
  potentially mutating access. Handle matcher implementations explicitly.
- Test helpers that build plans through fallible operations return
  `Result<FlashPlan>` and propagate errors without assertions or throwing to
  unwrap them. Helpers receiving or forwarding valid plans are allowed;
  helper names do not determine classification.
- Preserve ordinary container equality assertions initially. Extend detection
  to boolean equality algorithms and element-checking assertion loops; a
  universal matcher-only equality rule would be a separate convention change.
- Recognize uniform-byte checks from typed expressions: uniform `Each(...)`,
  constant-filled expected buffers and equivalent boolean algorithms. Recommend
  `BytesFilledWith` without guessing from names or buffer size.
- Use a versioned central exception file with check name, repository-relative
  file, enclosing test/helper identity and mandatory rationale. Reject malformed,
  duplicate and unused entries during full-scope verification. Changed-files
  verification does not classify entries outside its analyzed scope as unused.
  Exceptions use identities rather than line numbers.

## Assertion coverage

| Convention | Planned enforcement | Explicit limit |
| --- | --- | --- |
| Result/Status matchers | Recognize `std::expected<T, fastecu::Error>` through aliases; diagnose boolean state assertions and direct error-field assertions that match supported patterns | Ordinary optionals and low-level Result API tests remain allowed |
| Fatal guard before access | Intraprocedural success/error state analysis for dereference, arrow, and error access | Cross-function proofs and unknown aliases require documented handling; no claim of complete interprocedural analysis |
| No appended error detail | Diagnose Result error-detail streams appended to assertion messages | General message construction remains the migrated message check's responsibility |
| Inline single-use operations | Adjacent declaration/assertion pairs with preserved evaluation/destruction order | Ordering-sensitive and uncertain cases remain review responsibilities |
| Any-error assertions | Require a central rationale for `Not(IsOk())` | The check does not infer contracts from implementation |
| Plan helpers | Diagnose fallible plan builders that unwrap into `FlashPlan` or assert rather than propagate | Valid-plan forwarding remains allowed |
| Container assertions | Preserve migrated patterns; add equality algorithms and element-checking loops | Ordinary container `EXPECT_EQ` remains allowed initially |
| Uniform bytes | Recommend `BytesFilledWith` for statically recognized uniform checks | No size/name heuristics |
| Assertion loops | Require iteration-local traces for direct assertions and invoked lambdas | Separate helper calls remain a documented detection limit |
| Named parameterized cases | Recognize named instantiations, including `PrintToStringParamName` | Naming quality and expense are review responsibilities |
| Expensive transfer outcomes | Review requirement for separate parameterized cases | No expense heuristic in initial enforcement |

The coding guide remains the convention owner. During implementation, reconcile
its broad container wording with the explicit acceptance of ordinary equality,
and distinguish semantic guards from the literal `ASSERT_THAT` wording. Surface
these intended clarifications in review rather than silently changing the rule.

## Architecture and rollout

The existing Bazel-owned extension contains registered checks and shared
recognition of GoogleTest/GoogleMock constructs and Result types. Keep each
check's diagnostics and permitted counterexamples independently testable.
The runner selects the validated analysis host, builds the extension, and runs
built-in and repository checks with the unified header scope. Preserve existing
changed-file/platform selection, output deduplication, annotations, fix mode for
existing supported checks, and profiling. Missing checks fail before analysis.

Roll out in dependency order:

1. Qualify the pinned LLVM packaging, particularly Windows host symbol exports,
   headers and link libraries, with a build/load/diagnostic smoke test.
2. Establish mandatory extension builds and non-skipping fixtures on supported
   platforms. Resolve the existing plugin target's warning suppression by fixing
   the cause or treating external headers as external, following ADR 0018.
3. Migrate the nine YAML checks, proving equivalent diagnostics in equivalent
   scope before deleting each implementation. Update names and benchmark inputs.
4. Clean findings exposed by wider built-in header coverage, enable unified
   scope, and remove the experimental adapter and separate analysis pass.
5. Add assertion checks incrementally, cleaning existing findings before each
   becomes blocking. Keep checks with unresolved false positives disabled until
   their documented detection boundary is reliable.
6. Run full platform scope, fixtures, applicable Bazel/formatting/static-analysis
   gates and reproducible performance comparisons. Update tooling and coding
   conventions to describe the delivered behavior and remaining review duties.

Platform packaging is a qualification prerequisite, not an assumed capability.
If the pinned Windows host cannot load the extension, use a Bazel-built host
with linked checks as agreed; assess build cost and dependency availability
before finalizing its implementation plan.

Packaging amendment approved during qualification: the official Windows host
has no exports, and its static libraries require zlib/zstd absent from the
archive. Supply those dependencies through Bazel with compatible static Windows
runtime settings, retaining the official LLVM distribution. A real Windows
link/run smoke remains required before rollout.

## Integration facts and open decisions

The repository already has a compiled extension in
[`scripts/clang_tidy_plugin`](../../../scripts/clang_tidy_plugin/) with a
template-parameter check. It is optional locally and absent from CI. Extend or
replace this integration deliberately; avoid creating a competing extension.

The existing YAML checks and their fixtures define the migration's behavioral
baseline. Assertion coverage beyond that baseline is a separate change in
behavior and needs explicit positive and permitted-counterexample fixtures.

The documentation's absolute Windows loading prohibition needs qualification:
[upstream discussion](https://github.com/llvm/llvm-project/issues/159710)
describes loading with suitable symbol exports. It does not prove support in
the pinned executable. The packaging smoke test must establish that evidence.

## Review and handoff

Review this draft for shared understanding before implementation planning.
The implementation plan must turn the coverage limits into concrete fixtures,
choose an exception-file format and exact stable identities, and record verified
platform packaging. Those details must preserve the decisions above. A failed
platform probe or a need to broaden exception policy returns to design review.
