# ADR 0006: Verify Release Packaging in PR CI

## Status

Accepted and implemented.

Pull request CI and the release workflow now call the same Bazel-backed
packaging scripts for Windows and macOS.

## Context

Release packaging depends on platform deployment tools and runtime libraries
that an ordinary application build does not fully exercise. On Windows, the
package must contain the OpenSSL crypto DLL, Qt runtime DLLs, plugins, and the
32-bit J2534 bridge helper. On macOS, the application bundle must contain the
Qt frameworks and linked OpenSSL library.

The Windows OpenSSL installation can contain multiple `libcrypto-*.dll` files.
The selected runtime must match the headers and linked SONAME rather than an
arbitrary compatibility DLL.

## Decision

Pull request CI must build and package FastECU on every release platform and
upload reviewer artifacts. Release jobs must use the same scripts:

- `scripts/package-windows.ps1` builds Bazel targets, selects the configured
  OpenSSL DLL, runs `windeployqt`, includes the J2534 bridge helper, and asserts
  required runtime files are present.
- `scripts/package-macos.sh` builds `//:fastecu`, runs `macdeployqt`, and asserts
  that QtCore and OpenSSL were bundled.

Windows continues to select the highest matching `libcrypto-*.dll` SONAME from
the resolved OpenSSL installation.

## Consequences

- Packaging regressions fail before release and reviewers can test the actual
  platform archives against hardware.
- Pull requests and releases exercise the same package assembly logic.
- CI performs additional packaging work on Windows and macOS.
- Qt deployment tools and resolved OpenSSL paths remain platform inputs outside
  the Bazel graph, so CI assertions in the scripts remain necessary.
