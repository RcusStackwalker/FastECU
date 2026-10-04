# Eliminate valid high-severity Sonar findings

Date: 2026-10-04. Status: proposed design for review; implementation has not started.

## Goal and evidence

Remove every valid high-severity finding from the analyzed FastECU code while
preserving protocol behavior, ownership guarantees, and the existing layer
boundaries. The user explicitly permits evidence-backed, issue-specific Sonar
dispositions for necessary patterns. That permits designing the disposition
workflow; this investigation has made no server-side changes.

The [dated triage](../../sonar-triage-2026-10-04.md) and
[complete ledger](../../sonar-triage-2026-10-04.json) are the baseline: 2,161 open
issues, including 763 High-impact issues across 21 rules. All reported High
issues have maintainability impact. They are not 763 confirmed runtime defects.
Use the union of legacy Critical/Blocker and impact High/Blocker, deduplicated
by issue key. Counting Critical alone misses 111 `cpp:S7172` findings.

The latest server analysis is revision `8c1ac2b2`, three commits behind the
investigated checkout `6b17dbf9`. Twenty-nine High findings have changed source
files. All baseline locations are analysis locations, not guaranteed current
line numbers. This design is based on complete rule-level triage, source review
of representative instances and the small rule groups, and ownership-path
inspection. It does not certify individual validity for all 763 findings.

## Alternatives and recommendation

1. **Risk-first, bounded batches — recommended.** Repair demonstrated ownership
   defects first, take easy clarity improvements next, and refactor complex
   functions by subsystem. Dispose of justified exceptions individually. This
   delivers useful fixes early and keeps wire-sensitive changes reviewable.
2. **Count-first cleanup.** Start with the 178 macros and 278 mostly mechanical
   findings. This drops the headline count quickly but delays real leaks and
   invites unsafe automatic substitutions around Qt and status constants.
3. **One broad modernization.** Rewrite ownership and large UI/driver functions
   together. It may eliminate many findings at once, but mixes protocol changes,
   lifecycle changes and structural refactoring, making regressions difficult
   to isolate. Reject this option.

Use one program with separate package-sized PRs. No new general framework,
universal flash state machine, or backend-to-platform dependency is needed.
Respect the [coding conventions](../../coding-style.md),
[modularization plan](../../modularization-plan.md), and
[design notes](../../design-notes.md).

## Scope and accounting

| Workstream | Rules | Baseline findings | Relative effort |
|---|---|---:|---|
| Ownership | S5025 | 116 | Medium–large; disposition review dominates many sites |
| Targeted design | S3624, S859, S5008, S1242, S3656, S1699, S5421 | 20 | Small–medium per case |
| Clarity | S7172, S3608, S5019, Python S1192, S4962, S3490, S1186, S1709, shell S131 | 278 | Small per site, moderate aggregate |
| Constants | S5028 | 178 | Medium due to shared headers and conversions |
| Structure | C++ S3776/S134, Python S3776 | 171 | Large; several independent subsystems |
| **Total** | **21 rules** | **763** | |

Unless stated otherwise, rule IDs refer to C++. Counts are findings, not distinct
changes: one edit can resolve multiple rules at a location. Do not add per-rule
estimates as if fixes were independent.

Lower findings remain outside the zero-High objective. Review the 184 findings
in narrowing, shadowing, broad exception handling, loop correctness and regex
complexity ahead of cosmetic work where they touch the same code. They may
expose more practical risk than many High-rated style findings. Preserve the
exception translation required at ports; broad catches are not automatically
wrong. The remaining 1,214 lower findings are modernization or structural debt.

## W0 — Reconcile and record dispositions

Before editing, obtain a successful full Sonar analysis of the implementation
base through the existing build-wrapper workflow. Re-export every page and
record its revision, time, analysis ID and coverage exclusions. A fresh issue
query alone is not a fresh code analysis. Re-anchor the 29 changed-file findings
and any further drift by symbol and message, not line number alone.

Maintain an issue ledger containing the baseline key, rule, source anchor,
current applicability, action, evidence, PR/test reference and terminal result.
Use these states:

- `pending-review`: validity is unresolved; this never counts as cleared.
- `valid-fix`: current source violates the rule and needs an appropriate fix.
- `fixed`: code changed and subsequent analysis confirms resolution.
- `false-positive`: the rule's premise is demonstrably incorrect; record why.
- `justified-exception`: the rule correctly detects a pattern that is necessary
  here; record the contract, alternatives rejected, reviewer and evidence, and
  use the corresponding issue-specific Sonar disposition.
- `stale`: the finding no longer applies on the current revision; confirm on a
  fresh analysis before counting it as resolved.

An Accepted issue is not automatically invalid. Ordinary unimplemented fixes,
high implementation cost, or missing tests are not justified exceptions. Audit
Accepted issues at the final check as well as Open/Confirmed issues. Currently
there are zero Accepted issues.

Do not use `NOSONAR`, disable rules, raise complexity thresholds, relabel
production as tests, or expand exclusions to manufacture completion. Windows
sources and flash kernels are already excluded; preserve and report that
coverage limitation. Adding Windows analysis is a separate scope expansion,
and any newly analyzed findings need their own baseline.

## W1 — Ownership and cleanup guarantees

Audit all 116 S5025 sites. Record creation, ownership transfer, teardown,
early-return behavior, thread affinity and any borrower lifetime. An allocation
or `delete` alone is not evidence of a leak.

Concrete current-source priorities:

- [BIU operations](../../../src/ui/desktop/biu/biu_operations_subaru.cpp)
  allocate two QByteArrays and two QStringLists at lines 32–35; their destructor's
  deletes are commented out. Replace owned plain data with values or a single
  explicit RAII owner. Child result windows borrow those objects; close/destroy
  borrowers before their storage and prevent callbacks after destruction.
  Review unparented result windows in the same ownership pass.
- [BIU input2](../../../src/ui/desktop/biu/biu_ops_subaru_input2.cpp)
  creates unparented QButtonGroups and discards the local pointers. Give each
  group a QObject owner. Do not infer group ownership from its buttons being
  installed in a layout.
- [Unix J2534](../../../src/platform/desktop/unix/j2534/driver/J2534_unix.h)
  initializes `serial` with an unparented QSerialPort while its destructor is
  empty. Establish one owner and preserve IO-thread destruction. The crash
  harness currently nulls this pointer; update its teardown seam without
  leaking the displaced port or losing null-port regression coverage.
- [Settings](../../../src/ui/desktop/widgets/settings.cpp) creates a local
  `buttonsLayout` at line 37 and never installs or destroys it. Remove this
  unused allocation.

These are source-supported ownership defects, not runtime leak measurements.
Prove fixes with focused tests before claiming runtime validation.

Many other reports are candidates for justified exceptions: a table takes
ownership through `setCellWidget`/`setItem`, and QObject parents destroy their
children. The [Qt ownership documentation](https://doc.qt.io/qt-6/objecttrees.html)
and [QTableWidget contract](https://doc.qt.io/qt-6/qtablewidget.html#setCellWidget)
support these mechanisms. Verify the actual transfer on every exit path.
Do not install an independent smart-pointer owner over an object still owned
by Qt. Parentless does not mean leaked if ownership transfers later.

For serial host/factory code, preserve construction and destruction on the IO
thread and the existing shutdown order. A mechanically introduced unique_ptr
must not start deleting a backend on the GUI thread. A documented custom
deleter or existing thread-owned lifecycle can be appropriate.

The five S3624 scope guards must prohibit accidental duplication of cleanup:
`ChainGuard`, `StopGuard`, both semaphore `Completion` guards and `J2534IoScope`.
Delete copy operations and explicitly decide move policy; prefer nonmovable
guards where movement has no use. Add constructors where needed to preserve
initialization after changing aggregate status. Test exactly-once cleanup,
including errors/exceptions and cancellation, and use compile-time checks for
the ownership type traits.

Only default the 18 S3490 special members after this audit. In particular,
defaulting the Unix J2534 destructor before giving `serial` an owner would
silence one finding while retaining a leak.

## W2 — Targeted API and lifetime clarity

- **S859 (5):** preserve input constness in the Unix J2534 adapter. Cast to const
  SCONFIG_LIST, SBYTE_ARRAY, PASSTHRU_MSG or character pointers as appropriate;
  propagate constness into read-only dump helpers. Do not replace a C-style cast
  with a const_cast that retains the same defect. Preserve mutable outputs.
- **S5008 (3):** inspect the public PassThruOpen/PassThruIoctl boundary and its
  cross-platform contract. Keep necessary opaque ABI arguments with documented
  exceptions; perform type selection once and use typed internal helpers. A
  genuinely internal void pointer should become typed. Preserve identifiers,
  sizes and return codes; do not rewrite the vendor interface for a style rule.
- **S1242 (2):** rename the result-bearing worker `finished(result)` signals to
  an unambiguous name such as `operationFinished(result)`. Update emits,
  connections and tests together. Keep QThread::finished for thread shutdown;
  verify result delivery happens once and teardown still waits for completion.
- **S3656 (2):** protected driver fields support existing crash-test subclasses.
  Prefer private ownership plus a narrow existing injection/friend seam where
  that preserves test reach. If a new production hook would exist solely to
  satisfy this rule, a documented exception is preferable. Never drop the
  regression tests to make fields private.
- **S1699 (2):** explicitly qualify intended base operations in the WebSocket
  device and get-key dialog constructors, after verifying that construction-time
  dispatch is intended. Test their existing open/result behavior; move work out
  of construction only if its contract actually requires polymorphic dispatch.
- **S5421 (1):** the mutable `volatile std::sig_atomic_t` flag bridges SIGINT to
  cancellation. Propose a justified exception. Making it const defeats its
  purpose; replacing it with a mutex or arbitrary callback in the handler is
  not an acceptable cleanup. Retain handler restoration and cancellation tests.

## W3 — Mostly mechanical clarity

- **S7172 (111):** use `.has_value()` on std::expected/Result conditions,
  preserving polarity, early returns and error propagation. This already
  matches repository conventions. Do not replace presence checks with tests of
  the contained bool or value. Denso CAN executor code contains a large cluster.
- **S3608 (43), S5019 (35):** list captures explicitly, preserving reference vs
  value semantics and object lifetime. Synchronous diagnostic configuration
  lambdas are straightforward; queued/stored callbacks need lifetime review.
- **Python S1192 (31):** name genuinely shared fixture/config literals. Keep
  expected test values independent of the production implementation so a test
  does not start accepting an incorrect value merely by importing its constant.
- **S4962 (20):** change pointer null defaults to nullptr; use ordinary empty
  construction for QString/QByteArray when that expresses the intended value.
  Inspect aggregates field by field. The finding on the `STATUS_SUCCESS` macro
  is a macro-expansion issue: fix inappropriate pointer/string uses at their
  call sites, not the numeric status definition itself.
- **S3490 (18):** use `= default` once ownership is correct; retain out-of-line
  placement where incomplete types or existing linkage require it.
- **S1186 (12):** document intentional null-sink/compatibility bodies inside the
  body. An exposed empty UI action must be checked for missing behavior; remove
  dead wiring or make its unavailable state explicit rather than inventing an
  excuse. No new feature is required solely to eliminate an unused stub.
- **S1709 (6):** add explicit to conversion-capable constructors after checking
  callers, factories and Qt construction. Preserve supported direct construction.
- **shell S131 (2):** add explicit default no-op arms to the two filtering cases
  in coverage-local.sh. Unmatched files/paths currently continue normally;
  converting these defaults into errors would alter the script's contract.

Use existing focused tests and compile checks for these mechanical edits; do
not create one assertion per spelling change.

## W4 — Constants without conversion regressions

Address all 178 S5028 findings. Of these, 163 are in
[J2534_tactrix_unix.h](../../../src/platform/desktop/unix/j2534/J2534_tactrix_unix.h),
ten in serial facade codes, four in hex editor chunks, and one in MainWindow.

Inventory preprocessor uses, include consumers, aliases and conversions before
replacement. Use namespace-scoped constexpr values with deliberate integral
types; preserve numeric values and ABI structs. Retain a macro only when an
actual preprocessor/external contract requires it, with an issue-specific
exception. Do not alter ABI layout or on-wire values.

The facade's STATUS_SUCCESS name collides with Windows SDK macros. Use a new
collision-free identifier and migrate consumers together, rather than keeping
the same token as a constexpr. Preserve the distinct serial timing IDs and
J2534 IOCTL IDs; the existing P1_MAX static assertion guards one relationship.

Crucial precondition: direct serial `write_serial_data` returns STATUS_SUCCESS
through QByteArray's null-pointer constructor. The
[direct backend test](../../../src/platform/desktop/common/serial/direct/common/direct_backend_test.cpp)
explicitly pins this to an empty byte array. Replace these string/array return
sites with explicit empty values before converting the integer macro. Preserve
that behavior, not an accidental implicit conversion. Inspect bool/integer
returns independently. Keep the recently corrected unsigned hex-editor mask
and alignment behavior.

Validate numeric identities and representative combinations, header consumers
on Windows/macOS/Linux, direct-backend behavior and hex editor boundary tests.

## W5 — Structural cleanup with explicit behavior contracts

Clear 64 C++ complexity findings, 101 excessive-nesting findings and six Python
complexity findings. Current messages require C++ complexity <=25, Python <=15,
and nesting <=3. Confirm the active rule parameters at implementation time and
do not change them to pass. Sonar reanalysis determines compliance; local
function length is not a substitute for cognitive-complexity measurement.

Group S134 and S3776 on the same functions into a single refactor. Extract named
operations with coherent inputs/outputs; use guard clauses only when cleanup,
loop progression, event emission and mutation order are preserved.

| Subsystem | Design boundary and required regression evidence |
|---|---|
| Python tooling | Separate database validation, changed-file selection, replacement normalization and workflow stages. Preserve command arguments, diagnostics, return codes and failure handling with existing runner/Gazelle fixtures. |
| Config/definitions/calibration | Separate parsing, resolution and edit planning. Preserve malformed-input outcomes, expression results, addresses, write bounds and serialization. Keep the unresolved wrx02 behavior pinned; extracting its predicates does not require choosing a new predicate. |
| Flash plans/executors | Separate validation, handshake, block planning and transfer steps within each family. Preserve exact bytes, retries, timeouts, cancellation checks, erase scope, progress and errors with scripted transports. Do not unify families speculatively. |
| Unix J2534/direct serial | Separate message decoding, buffer accumulation and IOCTL dispatch from transport operations. Preserve partial reads, deadlines, reentrant teardown and closed/null-port behavior with existing mock OpenPort and crash tests. |
| UI/BIU/definition conversion | Extract conversion and selection logic from widget mutation. Definition conversion has a reported complexity of 256; use golden valid/malformed fixtures before decomposition. Preserve selections, signals and dialog behavior. |
| Hex editor | Refactor input/edit/render branches with existing undo/redo, insert/overwrite, selection and chunk-boundary tests. Reported complexities of 180 and 97 make this its own batch. |
| Logging/service/bench | Preserve stop/reconnect/cancellation order, error propagation and result events through fake ports and existing command fixtures. |

The [debt roadmap](../../tech-debt.md) currently says scattered complexity stays
in the backlog. On approval, update that policy for these identified High
findings: this program deliberately gives them a completion requirement.
Keep unrelated known behavior defects, such as wrx02 predicate selection and
DataTerminal delay semantics, separately tracked and pinned. If a refactor
exposes a defect, isolate its correction with reproduction and corrected
expectations rather than silently changing behavior within extraction.

## Validation and completion

Each batch must record focused package tests, relevant new behavioral tests,
and the Sonar keys resolved or individually disposed of. Ownership changes
need destruction/lifetime tests (QPointer or destruction counters where useful)
and leak/sanitizer checks where supported. Tests must cover meaningful failures,
not mirror the implementation.

Before integration, require the repository's release build/test gates, changed
C++ clang-tidy, applicable pre-commit checks, and Windows/macOS/Linux CI. Run
the portable-core gate for changed portable code. Packaging checks remain
required where lifecycle/header changes affect the application. Use the
[qualification matrix](../../flash-qualification-matrix.md) and linked bench
checklists for hardware-affecting behavior changes; automated tests do not
create a new hardware qualification.

Completion requires all of the following:

1. A successful full analysis of the exact integrated revision, with unchanged
   rule thresholds and no new exclusions used to hide findings.
2. All baseline keys reconciled, all newly introduced High/Blocker issues
   reviewed, and **zero valid outstanding High/Blocker or Critical findings**.
3. No pending-review items; every remaining Accepted/false-positive disposition
   has issue-specific evidence and review. Ordinary valid debt is not accepted
   to meet the target.
4. Every claimed code fix is verified by the analysis and relevant tests; a
   missing issue after a file move or exclusion is not sufficient evidence.
5. Report the analyzed revision, coverage blind spots, residual lower-severity
   count, disposition counts and test results. If any required validation is
   unavailable, completion remains unverified.

The existing Clean-as-You-Code gate must remain enabled, but passing it alone
does not prove zero High findings in old code. Start with explicit full-inventory
checks at each batch and at completion; no separate permanent ratchet service
is required by this proposal.

This is a proposed spec, not an approved implementation plan. The next design
decision is approval or amendment of the risk-first batches and their exit
criteria; implementation planning follows that review.
