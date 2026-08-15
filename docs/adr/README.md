# Architecture Decision Records

An ADR here records a decision about **structure**: what the build graph owns,
where a layering boundary sits, which system is the source of truth. It has a
status, it is dated by its number, and superseding one is an event worth
recording.

Coding conventions do not belong here. "Use `std::format`", "check `Result`
with `.has_value()`", "prefer gmock matchers" are rules with no lifecycle and
no trade-off left to revisit — they live in
[the coding style guide](../coding-style.md), which is one page and is the
normative source for style questions.

The dividing question: would changing this decision require changing how
components fit together, or only how a line of code is written? Only the first
is an ADR.

## Index

| ADR | Decision |
| --- | --- |
| [0001](0001-adopt-bazel-as-target-graph.md) | Bazel is the sole target build graph |
| [0002](0002-use-prek-for-fast-local-checks.md) | prek runs fast local formatting and repository checks |
| [0003](0003-use-github-actions-for-ci.md) | GitHub Actions is the canonical CI runner |
| [0004](0004-limit-qbytearray-to-qt-boundaries.md) | `QByteArray` is confined to Qt boundaries |
| [0005](0005-separate-platform-specific-backend-tests.md) | Platform-specific backend tests live in separate sources |
| [0006](0006-verify-release-packaging-in-pr-ci.md) | Pull request CI verifies release packaging |
| [0007](0007-use-bazel-as-ci-source-of-truth.md) | Bazel configuration is the CI source of truth |
| [0008](0008-use-package-owned-mocks.md) | Mocks are owned by the package defining the interface |

## Retired numbers

ADRs 0009 through 0014 were coding conventions recorded in ADR form. They were
folded into [the coding style guide](../coding-style.md); the files are gone
and the numbers are not reused. Their rationale is in Git history.

| Retired | Subject | Now in |
| --- | --- | --- |
| 0009 | `std::string_view` over `const char*` | Strings and messages |
| 0010 | gmock matchers for property assertions | Tests |
| 0011 | `std::format` for message construction | Strings and messages |
| 0012 | `std::ranges` over iterators and index loops | Collections |
| 0013 | `bytes::composeBe` for wire frames | Bytes and wire frames |
| 0014 | `.has_value()` over implicit `operator bool` | Error handling |

The next new ADR is 0015.
