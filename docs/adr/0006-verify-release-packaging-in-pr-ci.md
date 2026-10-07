# ADR 0006: Verify Release Packaging in PR CI

## Status

Accepted and implemented.

Pull request CI and the release workflow now build the same Bazel packaging
targets for Windows and macOS.

## Context

Release packaging depends on platform deployment tools and runtime libraries
that an ordinary application build does not fully exercise. On Windows, the
package must contain Qt runtime DLLs, plugins, and the 32-bit J2534 bridge
helper. On macOS, the application bundle must contain the Qt frameworks and
plugins.

## Decision

Pull request CI must build and package FastECU on every release platform and
upload reviewer artifacts. Release jobs must build the same targets:

- `//packaging:windows_zip` stages `//:fastecu` and the J2534 bridge helper,
  runs `windeployqt`, and asserts the Qt runtime was staged.
- `//packaging:macos_zip` stages `//:fastecu` in an app bundle, runs
  `macdeployqt`, and asserts that QtCore was bundled.

Both are built by the `qt_deploy_zip` rule, which takes the deploy tool from the
Qt tree `rules_qt` already downloads for the build, so CI installs no separate
Qt for packaging. The staging and zipping logic is `packaging/assemble.py`.

## Consequences

- Packaging regressions fail before release and reviewers can test the actual
  platform archives against hardware.
- Pull requests and releases exercise the same package assembly logic.
- CI performs additional packaging work on Windows and macOS.
- The Qt deployment tools are the ones from Bazel's own Qt, but they run as
  unsandboxed local actions that read the (multi-GB) Qt tree in place rather than
  declaring it as inputs, so the assertions in `packaging/assemble.py` remain
  necessary.
- The archives are reproducible for the same inputs: entries are sorted and
  carry fixed timestamps.
