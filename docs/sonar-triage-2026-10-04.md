# Sonar triage — 2026-10-04

This dated report is a server snapshot, not a new local scan. The [remediation spec](superpowers/specs/2026-10-04-sonar-high-severity-design.md) defines the proposed work; the [complete issue ledger](sonar-triage-2026-10-04.json) contains all 2,161 keys, messages, source locations, impact ratings, workstreams and Sonar links.

## Provenance and completeness

- Sonar CLI 1.4.0, project `RcusStackwalker_FastECU`, branch `master`, five pages of 500, 500, 500, 500 and 161 issues; 2,161 unique keys.
- Analysis: 2026-10-03 21:18:35 UTC, revision `8c1ac2b2b9c31d9e96a9176d3ab1104d4cc462dc`. Local HEAD: `6b17dbf9`, three commits newer.
- Legacy severities: 652 Critical, 1,011 Major, 496 Minor, 2 Info, zero Blocker. Impact severities: 763 High, 900 Medium, 496 Low, 2 Info, zero Blocker.
- All reported open findings are code smells with maintainability impact. Zero reported bugs, vulnerabilities, security hotspots, or Accepted issues. This does not establish absence of defects.
- High scope is the union of Critical/Blocker legacy severity and High/Blocker impact: **763 unique issues in 21 rules**. The additional 111 are `cpp:S7172`, reported as legacy Major.
- 29 high findings are in files changed since analysis: direct serial actions (15), hex editor chunks (7), BIU input2 (6), qtrohelper (1). Their server line numbers require re-anchoring. The remaining 734 have unchanged source files; unchanged source does not prove every finding is valid.
- Windows sources and flash kernels are excluded by the current configuration. Linux CFamily compilation does not cover all OS-specific code. A clean result must be described within that coverage.

## Ranking

Priority is based on practical impact, not Sonar's severity label. P0 = concrete lifetime defects; P1 = correctness/lifetime review; P2 = mostly mechanical cleanup; P3 = larger high-severity refactors; P4 = lower-severity backlog. Effort is relative, not a delivery estimate.

| Workstream | Issues | Priority | Effort | Practical impact |
|---|---:|---|---|---|
| lower-modernization | 1171 | P4 | Small–medium | Mostly readability/modernization; defer unless adjacent |
| structural | 171 | P3 | Large | Maintainability/testability; high regression exposure in wire and editing paths |
| mechanical | 278 | P2 | Small per site | Mostly clarity; inspect constructor/destructor and macro edge cases first |
| lower-correctness-review | 184 | P1 review | Small–large | Potential truncation, wrong assignment, exception-boundary or parser defects |
| ownership | 116 | P0/P1 | Medium–large | High where lifetime is unmanaged; Qt ownership requires case-by-case disposition |
| lower-structural | 43 | P4 | Medium–large | Maintainability; fold into touched functions |
| constants | 178 | P2 | Medium | Low runtime benefit; substantial count reduction; ABI and conversion care |
| targeted-design | 20 | P1 | Small–medium each | Cleanup guarantees, type safety and lifecycle clarity; some justified exceptions |

## All-rule inventory

Each issue belongs to exactly one row below and one workstream in the ledger. All high rules have explicit requirements in the spec. Lower-severity rules are ranked for follow-up; this proposal does not require clearing all 1,398 lower-impact findings. Rule-level triage and representative source checks are complete; per-issue validity review remains a requirement of implementation, especially for the 116 ownership findings.

| Rule | Count | High scope | Workstream | Representative message |
|---|---:|---|---|---|
| `cpp:S125` | 216 | No | lower-modernization | Remove the commented out code. |
| `cpp:S5028` | 178 | Yes | constants | Replace this macro by "const", "constexpr" or an "enum". |
| `cpp:S5827` | 141 | No | lower-modernization | Replace the redundant type with "auto". |
| `cpp:S6004` | 129 | No | lower-modernization | Use the init-statement to declare "exactly_bootstrap" inside the if statement. |
| `cpp:S5025` | 116 | Yes | ownership | Replace the use of "new" with an operation that automatically manages the memory. |
| `cpp:S7172` | 111 | Yes | mechanical | Use has_value() or another member function to clarify that the code tests the presence of a value in the "expected", not the contained "void" value itself. |
| `cpp:S134` | 101 | Yes | structural | Refactor this code to not nest more than 3 if/for/do/while/switch statements. |
| `cpp:S6177` | 86 | No | lower-modernization | Reduce verbosity with "using enum" for "fastecu::LogLevel". |
| `cpp:S5817` | 84 | No | lower-modernization | This function should be declared "const". |
| `cpp:S6022` | 76 | No | lower-modernization | Use "std::byte" for byte-oriented data manipulation. |
| `cpp:S5276` | 67 | No | lower-correctness-review | implicit conversion loses integer precision: 'int' to 'uint8_t' (aka 'unsigned char') |
| `cpp:S3776` | 64 | Yes | structural | Refactor this function to reduce its Cognitive Complexity from 28 to the 25 allowed. |
| `cpp:S5350` | 63 | No | lower-modernization | Make the type of this variable a pointer-to-const. The current type of "session" is "class fastecu::calibration::CalibrationSession *". |
| `cpp:S3230` | 47 | No | lower-modernization | Do not assign data members in a constructor. Initialize member "link" in an initialization list. |
| `cpp:S3608` | 43 | Yes | mechanical | Explicitly capture the required scope variables. |
| `cpp:S2738` | 42 | No | lower-correctness-review | "catch" a specific exception type. |
| `cpp:S1181` | 37 | No | lower-correctness-review | Catch a more specific exception instead of a generic one. |
| `cpp:S5019` | 35 | Yes | mechanical | Explicitly capture all local variables required in this lambda. |
| `cpp:S995` | 32 | No | lower-modernization | Make the type of this parameter a reference-to-const. The current type of "request" is "struct fastecu::flash::FlashWorkflowRequest &". |
| `python:S1192` | 31 | Yes | mechanical | Define a constant instead of duplicating this literal ".clang-tidy" 8 times. |
| `cpp:S1117` | 25 | No | lower-correctness-review | Declaration shadows a field "protocol" in the outer scope. |
| `cpp:S3471` | 25 | No | lower-modernization | Annotate this function with "override" or "final". |
| `cpp:S6045` | 22 | No | lower-modernization | Use the transparent comparator "std::less<>" with this associative string container. |
| `cpp:S5566` | 21 | No | lower-modernization | Change this raw for-loop to a range for-loop or an "std::ranges::for_each". |
| `cpp:S1172` | 21 | No | lower-modernization | Remove the unused parameter "context", make it unnamed, or declare it "[[maybe_unused]]". |
| `cpp:S4962` | 20 | Yes | mechanical | Use the "nullptr" literal. |
| `cpp:S7035` | 18 | No | lower-modernization | Use "std::to_underlying" to cast enums to their underlying type. |
| `cpp:S3490` | 18 | Yes | mechanical | Use "=default" instead of the default implementation of this special member functions. |
| `cpp:S107` | 15 | No | lower-structural | This function has 8 parameters, which is greater than the 7 authorized. |
| `cpp:S1238` | 14 | No | lower-modernization | Pass large object "outcome" by reference to const. |
| `cpp:S6197` | 13 | No | lower-modernization | Replace with the version of "std::ranges::copy" that takes a range. |
| `cpp:S1659` | 13 | No | lower-modernization | Define each identifier in a dedicated statement. |
| `cpp:S3358` | 13 | No | lower-modernization | Extract this nested conditional operator into an independent statement. |
| `cpp:S1066` | 12 | No | lower-modernization | Merge this "if" statement with the enclosing one. |
| `cpp:S886` | 12 | No | lower-correctness-review | Refactor this loop so that it is less error-prone. |
| `cpp:S1186` | 12 | Yes | mechanical | Add a nested comment explaining why this method is empty, or complete the implementation. |
| `cpp:S3539` | 12 | No | lower-modernization | Remove this redundant access specifier; it does not change the accessibility level. |
| `cpp:S1820` | 10 | No | lower-structural | Refactor this structure so it has no more than 20 fields, rather than the 24 it currently has. |
| `cpp:S2209` | 10 | No | lower-modernization | Replace "." with "::" for access to "QByteArray::fromHex". |
| `cpp:S1188` | 8 | No | lower-structural | This lambda has 28 lines, which is greater than the 20 lines authorized. Split it into several lambdas or functions, or make it a named function. |
| `cpp:S924` | 7 | No | lower-modernization | Reduce the number of nested "break" statements from 2 to 1 authorized. |
| `cpp:S1448` | 7 | No | lower-structural | Class has 175 methods, which is greater than the 35 authorized. Split it into smaller classes. |
| `cpp:S1155` | 7 | No | lower-modernization | Use "isEmpty()" to check whether the container is empty or not. |
| `python:S3776` | 6 | Yes | structural | Refactor this function to reduce its Cognitive Complexity from 24 to the 15 allowed. |
| `cpp:S6030` | 6 | No | lower-modernization | Replace this use of "emplace" with "try_emplace". |
| `cpp:S1709` | 6 | Yes | mechanical | Add the "explicit" keyword to this constructor. |
| `cpp:S7040` | 6 | No | lower-modernization | Use bounded syntax to provide numerical escape sequences |
| `python:S5778` | 5 | No | lower-modernization | Refactor this exception test to have only one invocation possibly throwing an exception. |
| `cpp:S6003` | 5 | No | lower-modernization | Replace this use of "push_back" with "emplace_back". |
| `cpp:S1905` | 5 | No | lower-modernization | Remove this redundant cast. |
| `cpp:S3624` | 5 | Yes | targeted-design | Customize this class' copy constructor to participate in resource management (the copy constructor is implicitly created). |
| `cpp:S859` | 5 | Yes | targeted-design | C-style cast removing const qualification from the type of a pointer may lead to undefined behavior. |
| `cpp:S1103` | 5 | No | lower-modernization | Remove the misleading "//" characters. |
| `cpp:S1121` | 5 | No | lower-modernization | Extract the assignment to "startUpSplashProgressBarValue" from this expression. |
| `cpp:S6225` | 4 | No | lower-modernization | Use "std::to_address" to convert iterator to pointer. |
| `cpp:S5820` | 4 | No | lower-modernization | Convert this integer literal to a bool literal. |
| `cpp:S1110` | 4 | No | lower-modernization | Remove these redundant parentheses. |
| `cpp:S1116` | 4 | No | lower-modernization | Remove this empty statement. |
| `cpp:S5416` | 4 | No | lower-modernization | "using" should be preferred to "typedef" for type aliasing. |
| `cpp:S6012` | 3 | No | lower-modernization | Avoid explicitly specifying the template arguments by relying on the class template argument deduction. |
| `cpp:S3574` | 3 | No | lower-modernization | Remove the redundant return type of this lambda. |
| `cpp:S5414` | 3 | No | lower-modernization | Don't mix public and private data members. |
| `cpp:S108` | 3 | No | lower-modernization | Fill this compound statement, remove it, or add a nested comment explaining why it is empty. |
| `cpp:S5008` | 3 | Yes | targeted-design | Replace this use of "void *" with a more meaningful type. |
| `cpp:S6009` | 2 | No | lower-modernization | Replace this const reference to "std::string" by a "std::string_view". |
| `shelldre:S131` | 2 | Yes | mechanical | Add a default case (*) to handle unexpected values. |
| `cpp:S1242` | 2 | Yes | targeted-design | Rename this member function so that it doesn't hide an inherited non-virtual function, or make it virtual in the base class "QThread". |
| `cpp:S3628` | 2 | No | lower-modernization | Convert this string literal to a raw string literal. |
| `python:S1481` | 2 | No | lower-modernization | Replace the unused local variable "commands" with "_". |
| `cpp:S1135` | 2 | No | lower-modernization | Complete the task associated to this "TODO" comment. |
| `cpp:S3646` | 2 | No | lower-modernization | Declare this variable in a separate statement. |
| `cpp:S3642` | 2 | No | lower-modernization | Replace this "enum" with "enum class". |
| `cpp:S3656` | 2 | Yes | targeted-design | Member variables should not be "protected". |
| `cpp:S4144` | 2 | No | lower-structural | Update this method so that its implementation is not identical to select_protocol_finished. |
| `cpp:S1699` | 2 | Yes | targeted-design | This call always selects "QIODevice::open" without considering overrides in subclasses. Ensure the code unambiguously uses the desired function. |
| `python:S8786` | 1 | No | lower-correctness-review | Simplify this regular expression to reduce its runtime, as it has super-linear performance due to backtracking. |
| `python:S108` | 1 | No | lower-modernization | Either remove or fill this block of code. |
| `cpp:S6484` | 1 | No | lower-modernization | Replace concatenated std::format calls with a single invocation. |
| `cpp:S5421` | 1 | Yes | targeted-design | Global variables should be const. |
| `cpp:S3732` | 1 | No | lower-modernization | Move this 'extern "C"' declaration out of the namespace. |
| `cpp:S5415` | 1 | No | lower-modernization | "std::move" should not be called on a const object. |
| `cpp:S6178` | 1 | No | lower-modernization | Use "starts_with()" to check the prefix of the string. |
| `cpp:S6181` | 1 | No | lower-modernization | Replace "std::memcpy" invocation with "std::bit_cast". |
| `cpp:S6226` | 1 | No | lower-modernization | Empty class members should be marked as "[[no_unique_address]]" |
| `cpp:S5945` | 1 | No | lower-modernization | Replace this C-style array with "std::vector" (for dynamic size), or "std::array" (for static size) |
| `cpp:S1479` | 1 | No | lower-structural | Reduce the number of switch cases from 50 to at most 30. |

## Reproduce the inventory

Run the following for each page until `paging.total` unique keys are collected. Do not stop at the default 500. Query without a severity filter, then apply the union described above; this avoids losing High-impact findings mapped to Major.

```sh
sonar list issues --project RcusStackwalker_FastECU --branch master \
  --statuses OPEN,CONFIRMED --page-size 500 --page 1 --format json
sonar api get '/api/project_analyses/search?project=RcusStackwalker_FastECU&branch=master&ps=1'
sonar api get '/api/issues/search?componentKeys=RcusStackwalker_FastECU&branch=master&resolved=false&ps=1&facets=severities,impactSeverities,types,rules,issueStatuses'
sonar api get '/api/hotspots/search?projectKey=RcusStackwalker_FastECU&branch=master&ps=100'
sonar list issues --project RcusStackwalker_FastECU --branch master --statuses ACCEPTED --format json
```

Paginate the Accepted and hotspot queries too if their totals exceed a page. Check the analysis revision before and after retrieval; restart if it changed. Store only issue metadata, never credentials. No Sonar statuses were changed during this triage.
