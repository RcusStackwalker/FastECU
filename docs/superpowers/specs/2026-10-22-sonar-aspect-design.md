# Sonar C++ input generation through Bazel aspects

Status: approved on 2026-10-22. Implementation and qualification follow the
[implementation plan](../plans/2026-10-22-sonar-aspect-migration.md).

## Intent and agreed scope

Following the [clang-tidy aspect migration](../../tooling.md#clang-tidy), make
Bazel own Sonar C++ input generation and validation. Bazel ownership is the
primary goal; measured runtime improvement is an acceptance criterion rather
than an assumed consequence of using aspects.

The user accepted these boundaries on 2026-10-22:

- Start with C++ analysis-input generation and validation.
- Preserve coverage collection and import, other-language analysis, and the
  blocking cloud Quality Gate.
- Preserve the current Linux-only Sonar analysis scope. Windows analysis is
  separate work; its existing exclusion remains explicit.
- Separate coverage production from scanning. The scanner consumes a report
  artifact for the exact checked-out commit from the same workflow run and
  rejects missing or mismatched reports.
- Keep Hedron available for editor integration; Sonar gets its own input path.
- Use `--config=coverage` for the candidate inputs during qualification and
  initial cutover, preserving current compiler selection, flags and defines.
- Measure input preparation separately. Require repeatable warm improvement
  and no material whole-workflow regression; establish numerical thresholds
  from baseline measurements before candidate acceptance.
- Own a small aspect reading actual configured compile actions rather than
  adding Pigweed or retaining action-query extraction in the Sonar path.
- Use two jobs within the existing reusable workflow: coverage first, then
  input preparation and scanning. A third parallel preparation job is deferred
  unless measurements justify its extra runner and output handoff.
- Provide `bazel run //:sonar_inputs` for the full host-compatible graph, using
  the coverage configuration and a separate Sonar database. Support Linux and
  local macOS input preparation; Windows remains outside this migration.
- Require configured-command parity, generated-input and remote-cache checks,
  a real Sonar scan, and measured performance before replacing the current path.

## Current pipeline and supported integration

The [Sonar workflow](../../../.github/workflows/sonar.yml) collects coverage,
generates a compilation database using
[Hedron](../../../bazel/compile_commands/BUILD.bazel) under the coverage
configuration, checks that Qt headers resolve, and runs the scanner and cloud
Quality Gate. The [tooling guide](../../tooling.md#sonarcloud) owns the current
operator contract; [ADR 0017](../../adr/0017-generate-bazel-targets-with-gazelle.md)
records the retained compilation-database dependency.

The pinned Hedron implementation extracts configured compiler actions using
`bazel aquery`; it does not use an aspect. It also serves editors independently
of Sonar.

Sonar documents a [compilation-database input followed by a scanner invocation](https://docs.sonarsource.com/sonarqube-cloud/analyzing-source-code/languages/c-family/running-the-analysis),
including [third-party database generators](https://docs.sonarsource.com/sonarqube-cloud/analyzing-source-code/languages/c-family/prerequisites).
The reviewed documentation does not establish a standalone per-source analyzer
or report-merging API. Therefore this design must not promise clang-tidy-style
per-source Sonar analysis actions or Bazel caching of cloud analysis. The
scanner owns its [incremental analysis cache](https://docs.sonarsource.com/sonarqube-cloud/analyzing-source-code/languages/c-family/customizing-the-analysis#analysis-cache).

Generated and virtual include headers must be accessible to the scanner even
when build actions come from a remote cache. The current
[Qt guard](../../../scripts/sonar_compdb_check.py) checks that at least one
database include directory resolves `QtCore/qobjectdefs.h`; this is narrower
than proving every configured translation unit has all of its required headers.

## Scope discrepancy requiring qualification

The tooling guide describes C++ suites and fixtures as analyzed tests. However,
Sonar's [CFamily test-file documentation](https://docs.sonarsource.com/sonarqube-cloud/analyzing-source-code/languages/c-family/customizing-the-analysis#analyzing-test-files)
says that `sonar.tests` is disregarded by CFamily and test files must enter
`sonar.sources` to be analyzed. The current
[classification](../../../sonar-project.properties) excludes test patterns from
production scope.

Record this disagreement before reconciling the guide. Actual scanner evidence
must establish the existing effective C++ scope; database membership and test
classification alone do not prove that Sonar analyzes a file. The migration's
agreed preservation boundary does not authorize expanding that scope.

## Agreed coverage job separation

The user accepted separate coverage and scanner jobs with an exact-commit
report artifact on 2026-10-22. The preservation requirement concerns coverage
behavior and import, not keeping both operations on one runner. Input
preparation runs in the scanner job after coverage.

Keep both jobs inside the existing reusable workflow, preserving its `ref`
and `cache-save` inputs and its PR and release callers. The coverage producer
resolves `ref` to a commit and exposes that SHA along with the artifact identity.
The scanner checks out that exact SHA. This is important for release callers,
whose supplied tag can identify a different commit from the workflow event SHA.
Keep the existing blocking scanner and Quality Gate check identifiable through
the split.

With a reliable compilation database, Sonar does not need application object
files or linked binaries. It does need the referenced sources, generated files,
headers and toolchain resources. The aspect path should request the required
generation actions and materialize their results without requesting ordinary
C++ compilation or application linking. This remains a qualification obligation
for the selected graph, rather than evidence that an arbitrary C++ graph has no
compiled prerequisites.

A separate coverage producer still compiles and runs instrumented tests.
The agreed handoff is a
[workflow artifact](https://docs.github.com/en/actions/tutorials/store-and-share-data)
from the same workflow run and exact checked-out commit, containing the final
LLVM text report and metadata needed to validate its identity and paths.
Existing Bazel action caching continues to accelerate compilation and linking.
GitHub distinguishes [artifacts from dependency caches](https://docs.github.com/en/actions/concepts/workflows-and-actions/dependency-caching):
the report is a required job output, rather than a best-effort branch-cache
restore. Cross-run report reuse would require a separate validity contract.

The artifact contains only the completed LLVM text report, its header manifest
and a versioned provenance manifest. Record the resolved commit, workflow run
and producing attempt, producer checkout root, coverage configuration and
toolchain identity, plus digests of the report and header manifest. The scanner
validates this metadata against its checkout and the expected producer output
before import. Changing these inputs invalidates reuse; no branch-cache
fallback supplies an older report.

Use an immutable artifact identity exposed by the successful coverage job,
with a distinct artifact on a new producer attempt. A scanner-only rerun may
reuse the earlier successful producer artifact from the same workflow run and
commit. The producer attempt is provenance, not a requirement to equal the
scanner's current attempt. Missing artifacts, identity mismatches, digest
failures and malformed metadata fail before scanning.

The current [coverage script](../../../scripts/coverage-local.sh) rewrites
report section headers to producer-checkout-absolute paths. Before import on
another runner, localize those paths to the scanner checkout and validate that
they resolve to indexed project files. Transfer of the report alone does not
establish importability. Raw profiles, test binaries and the producer's Bazel
output tree are unnecessary for importing the completed text report.

Relocate only file-section headers under the recorded producer root; do not
replace arbitrary report text. Preserve already external paths. Regenerate or
validate the relocated header manifest and retain the existing minimum of 100
resolvable first-party sections. Real scanner evidence must additionally
confirm coverage import against indexed files; the local count alone does not
prove it.

The script also disables Bazel test-result caching: raw profiles are written
outside declared test outputs. Enabling cached test results alone could skip
test execution without restoring the profiles needed to produce a fresh report.

Separating jobs provides independent failure and rerun boundaries. It does not
remove coverage work or inherently shorten the critical path: the final scan
must wait for its report. Input preparation can overlap coverage in a separate
job if the measured saving justifies another runner and output handoff.

## Agreed extraction approach

Use a small repository-owned aspect that reads the configured `CppCompile`
actions, emits declared compilation-database fragments, and requests the
required generated inputs. This avoids reconstructing compiler flags or
bringing in another large dependency. The wrapper aggregates declared outputs
and localizes paths for a scanner invocation on the same runner. The user
accepted this approach on 2026-10-22.

Bazel exposes [action arguments and inputs](https://bazel.build/rules/lib/builtins/Action).
Upstream [C++ integration tests](https://bazel.googlesource.com/bazel/+/f057c7408a60cb9955a95d02e9e9a05760bd1f13/src/test/shell/bazel/cc_integration_test.sh)
exercise access through `target.actions`. The API distinguishes analysis-time
`argv` from execution-time `Args`; response files and directory expansion
require qualification rather than assuming every argument is already expanded.

The considered alternatives were adopting Pigweed's existing
compilation-database aspect, which adds a broader dependency and IDE-oriented
behavior, or retaining action-query extraction with an aspect used only to
materialize inputs. The latter retains the existing extraction boundary and
achieves less of the requested migration.

Do not copy clang-tidy's flag rewriting or generated-source exclusion into the
new database by default. Hedron currently includes generated first-party C++
compile actions; the candidate must establish parity against the current Linux
baseline before any deliberate filtering. Source paths alone are insufficient
identity when one source has multiple owner targets or configurations.

Traverse C++ dependencies and the forwarding edges needed to reach configured
compile owners. Keep distinct owner/configuration instances rather than
deduplicating by filename. Exclude external owner targets as the baseline does;
their headers and toolchain resources remain available to first-party commands.
Preserve argument ordering, defines, language mode, include paths and compiler
selection. Normalize only documented path representations needed to make the
database usable outside a sandbox. Unsupported command or response-file forms
must fail explicitly rather than silently omit an entry.

Expose fragments and required generated inputs through declared aspect output
groups. The wrapper resolves declared outputs through build events, downloads
them when remote caching is enabled, and writes a deterministic merged
database localized to the current execution root. Merely mentioning an include
path in JSON does not materialize the referenced files. Request the input
closure explicitly without requesting application object or binary outputs.

The scanner keeps running outside cacheable Bazel actions, with its existing
authentication and Quality Gate check. Compiler and installed system-resource
availability remain runner setup requirements; this is not a claim that Sonar
or host installations become hermetic.

## Local command and failures

`bazel run //:sonar_inputs` prepares and validates the full host-compatible
first-party C++ graph under `--config=coverage`. Its output is
`sonar-inputs/compile_commands.json`, separate from the editor's root
`compile_commands.json`. The output directory is ignored by Git. The command
does not require Sonar credentials or submit a scan; local coverage collection
and scanning remain explicit steps documented in the tooling guide.

Linux is the CI qualification platform. Local macOS input preparation also
receives a smoke check against its host-selected graph and SDK. No new macOS
analysis CI job or Windows Sonar support is added.

Fail the command on build failures, missing declared outputs, invalid command
fragments, an empty eligible graph, missing referenced source/generated inputs,
or failed Qt-header resolution. Validate complete configured-command
collection, including forwarding outputs, rather than treating any nonempty
JSON file as sufficient. Publish the final database only after validation;
a failed invocation must not leave an older database looking current.

The scanner job also rejects unusable coverage artifacts before invoking
Sonar. Preserve scanner failures and the blocking cloud Quality Gate; never
fall back silently to Hedron or an older coverage report. An explicit rollback
to the existing workflow is a reviewable change if qualification fails.

## Qualification and cutover

The migration is unqualified until recorded evidence establishes the following:

| Boundary | Required evidence |
| --- | --- |
| Configured scope and commands | Compare the current Linux Hedron baseline with aspect output under the same coverage configuration. Match source/owner/configuration identities and normalized ordered arguments, including generated sources and repeated configurations. Explain every allowed normalization. |
| Fresh preparation | Use a fresh scanner setup without a preceding coverage build on that runner. Required generated sources, Qt headers and include trees resolve. The execution plan for the requested outputs schedules no application/test compile or link actions, including actions satisfied from cache; inspecting their metadata is permitted. Building a required generation tool is distinct and must be reported. |
| Remote output availability | Prepare from a remote cache with minimal downloading and a fresh local output base. Declared fragments and generated inputs are available to the scanner, including virtual include symlinks and their targets. |
| Failure handling | Exercise empty or incomplete output collection, invalid fragments, missing generated headers, unsupported response-file forms, missing/mismatched/corrupt coverage artifacts and failed path relocation. Each fails before submitting an incomplete scan. |
| Artifact portability and reruns | Import a report produced at another checkout root. Verify its provenance and digests, relocated first-party paths and preserved external paths. A scanner rerun reuses the successful same-run producer artifact; a new producer attempt has a distinct identity. |
| Actual analysis | Run the real scanner and Quality Gate. Confirm effective C++ scope, findings and imported coverage against the existing pipeline, preserving other-language analysis and the Windows exclusion. Record analyzer/profile identity and explain differences rather than assuming the server stayed unchanged. Resolve the documented C++ test-scope disagreement using this evidence. |
| Local macOS | Run input preparation on the host-selected graph and verify generated inputs and SDK paths without an application prebuild. This does not substitute for Linux qualification. |
| Performance | Record comparable baseline/candidate input-preparation timings for cold, unchanged warm and one-source-change runs, plus whole-workflow time including the job split and artifact transfer. Require repeatable warm improvement and no material whole-workflow regression. |

Performance thresholds are established and recorded from baseline measurements
before candidate acceptance. Keep runner, graph, configuration and cache
conditions comparable, and report variability rather than relying on one
sample. Record input preparation separately from instrumented tests, scanner
execution and cloud processing. Splitting jobs is not itself evidence of a
speedup.

Retain the existing blocking path during qualification. A temporary comparison
is acceptable; do not leave a permanent duplicate advisory job. After the
evidence passes, replace Sonar's input-generation step and split coverage in the
same reusable workflow, keeping scanner and Quality Gate enforcement blocking.
Hedron and its Bazel compatibility patch remain for editors.

The implementation updates the owning tooling guide, the Sonar properties,
the relevant compilation-database paragraph in ADR 0017, and a focused
qualification record. Link to those owners instead of copying inventories.
Do not rewrite the current operator contract as if a proposed path were already
implemented. After delivery, preserve this spec's original contents in retained
Git history before removing it according to the documentation index.

## Review boundary

The interview frontier is empty: scope, extraction, configuration, editor
retention, job layout, local command, artifact behavior and acceptance evidence
have been agreed. The user approved the consolidated design and selected native
execution after plan preparation. Baseline measurements and qualification
results are future evidence, not claims established by this design.
