# ADR 0006: Verify Release Packaging in PR CI

## Status

Accepted and implemented.

Pull request CI and the release workflow now call the same Bazel-backed
packaging scripts for Windows and macOS.

## Context

Release packaging depends on platform deployment tools and runtime libraries
that an ordinary application build does not fully exercise. On Windows, the
package must contain Qt runtime DLLs, plugins, and the 32-bit J2534 bridge
helper. On macOS, the application bundle must contain the Qt frameworks and
plugins.

## Decision

Pull request CI must build and package FastECU on every release platform and
upload reviewer artifacts. Release jobs must use the same scripts:

- `scripts/package-windows.ps1` builds Bazel targets, runs `windeployqt`,
  includes the J2534 bridge helper, and asserts required runtime files are present.
- `scripts/package-macos.sh` builds `//:fastecu`, runs `macdeployqt`, and asserts
  that QtCore was bundled.

## Consequences

- Packaging regressions fail before release and reviewers can test the actual
  platform archives against hardware.
- Pull requests and releases exercise the same package assembly logic.
- CI performs additional packaging work on Windows and macOS.
- Qt deployment tools remain platform inputs outside the Bazel graph, so CI
  assertions in the scripts remain necessary.
