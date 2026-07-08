# ADR 0005: Separate Platform-Specific Backend Tests

## Status

Accepted

## Context

Backend and backend-test code needs to build across Windows, Linux, and macOS.
Some tests exercise platform facilities directly, such as Unix pseudo-terminals
via `openpty`. Keeping those tests in common source files requires
preprocessor guards around includes, test declarations, helpers, and test
bodies. That makes the common backend tests harder to read and easier to break
on platforms that do not provide those APIs.

FastECU uses Qt, but backend code should not depend on Qt platform macros when
standard source layout or standard compiler/platform checks are enough.

## Decision

Prefer source file separation for platform-specific backend tests.

Platform-specific tests should live in files that are included only by the
matching qmake and Bazel source lists. Common backend test files should contain
only tests that compile on all supported platforms.

Where source file separation is not feasible, use small preprocessor guards to
determine the platform. Prefer standard compiler/platform macros for backend
and backend-test code. Keep Qt-specific platform macros to a minimum outside
Qt-boundary code.

## Consequences

Positive consequences:

- Common backend tests stay readable and portable.
- Windows builds do not parse Unix-only headers or helpers.
- Platform coverage remains available without weakening the common test target.
- qmake and Bazel source lists make platform ownership explicit.

Negative consequences and risks:

- Platform-specific tests require a small runner hook from common test mains.
- qmake and Bazel source manifests must stay in sync when platform test files
  are added or moved.
- A platform-specific source file may still need a small local macro to select
  headers within that platform family, such as Linux `<pty.h>` versus macOS
  `<util.h>`.

## Guardrails

- Do not add `openpty` or other Unix-only API tests to common backend test
  sources.
- Add Unix-only backend tests to the `*_UNIX_SRCS` and `*_UNIX_HDRS` Bazel
  lists and the qmake `unix {}` block.
- Keep common test runners limited to one small platform branch when they must
  call a platform-specific test entry point.
- Run the openpty include/source-separation check and source-list sync checks
  when moving platform-specific tests.
