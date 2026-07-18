# ADR 0005: Separate Platform-Specific Backend Tests

## Status

Accepted and implemented in the Bazel target graph.

## Context

Backend and backend-test code must build across Windows, Linux, and macOS.
Some tests exercise platform facilities directly, such as Unix
pseudo-terminals via `openpty`. Keeping those tests in common sources requires
preprocessor guards around includes, declarations, helpers, and test bodies,
which makes the common suites harder to read and easier to break elsewhere.

FastECU uses Qt, but backend code should not depend on Qt platform macros when
source ownership or standard compiler/platform checks are sufficient.

## Decision

Prefer separate source files for platform-specific backend tests. Include those
files only in the matching Bazel configured source list. Common backend test
sources must compile on every supported platform.

Where source separation is impractical, use a small local preprocessor guard.
Prefer standard compiler/platform macros outside Qt-boundary code.

## Consequences

Positive consequences:

- Common backend tests remain readable and portable.
- Windows builds do not parse Unix-only headers or helpers.
- Platform coverage remains available without weakening the common test target.
- Bazel compatibility and configured source lists make platform ownership
  explicit.

Costs and risks:

- Platform-specific suites need a small runner hook from common test mains.
- A platform source may still need a local choice such as Linux `<pty.h>` versus
  macOS `<util.h>`.
- New platform files must be added to the correct `*_UNIX_*` or `*_WIN32_*`
  manifest and exercised on that CI platform.

## Guardrails

- Do not add `openpty` or other Unix-only APIs to common backend test sources.
- Add Unix-only backend tests to the relevant `*_UNIX_SRCS` and
  `*_UNIX_HDRS` Bazel lists.
- Keep common test runners limited to one small platform branch when they must
  call a platform-specific entry point.
- Run `//:openpty_includes` and the Windows/macOS/Linux Bazel test matrix when
  moving platform-specific tests.
