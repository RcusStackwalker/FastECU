# ADR 0006: Verify Release Packaging in PR CI

## Status

Accepted

## Context

Release packaging depends on platform-specific deploy tools and runtime
libraries that ordinary application builds do not fully exercise. On Windows,
the release package must include the OpenSSL crypto DLL copied from the
Chocolatey OpenSSL installation before `windeployqt` completes the portable
bundle.

The OpenSSL installer can place more than one `libcrypto-*.dll` in the same
`bin` directory when it keeps a compatibility DLL beside the current runtime
DLL. The headers in the selected OpenSSL installation match the newest DLL
SONAME, so choosing an arbitrary file can package an older runtime.

## Decision

Pull request CI must build and package FastECU on every release platform, then
upload one reviewer artifact per platform with a shared artifact naming and
retention policy.

Windows packaging must select the highest `libcrypto-*.dll` SONAME from the
resolved OpenSSL installation. The PR workflow should package Windows with the
same static qmake and `windeployqt` path used by the release workflow.

## Consequences

Positive consequences:

- Packaging regressions fail before a public release.
- Reviewers can download the same style of package that users receive and test
  it against real hardware before merging.
- Artifact upload behavior stays uniform across Windows and macOS.
- Workflow comments can stay operational because the packaging rationale lives
  in this ADR.

Negative consequences and risks:

- PR CI does extra packaging work on Windows and macOS.
- The qmake release-packaging path remains duplicated until release packaging
  moves behind a shared script or Bazel-backed artifact.
