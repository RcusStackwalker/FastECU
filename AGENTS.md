# FastECU agent instructions

FastECU is a Qt 6/C++23 desktop application for Subaru and Mitsubishi ECU/TCU
reading, flashing, and logging. This independently maintained fork uses Bazel
as its sole application, test, packaging, coverage, and analysis graph.

## Read for the task

Use the [documentation index](docs/README.md) to find the relevant decision,
contract, convention, and qualification record. Read the relevant section and
follow detail links when needed; structural work does not require reading all
roadmaps or design history. Proposed specs provide context, not authorization
to implement them.

## Build and verify

The Bazel version is pinned in `.bazelversion`. Host Qt/tool setup is defined
by [PR CI](.github/workflows/pr.yml). Common commands:

```sh
bazel build --config=release //:fastecu
bazel test --config=release //...
prek run --all-files
python3 scripts/gazelle_check.py --fix
bazel run //:clang_tidy_report_changed
```

Use focused package tests during code changes; run the applicable full gates
before delivery. Formatting, Gazelle, [static analysis](docs/coding-style.md#static-analysis),
and platform CI/packaging are established gates. Markdown-only work uses
`prek run lychee --all-files` and a scoped diff review; do not build the application
for a documentation-only change.

## Essential constraints

- Backend and algorithms are portable: no Qt, owned threads, direct filesystem
  I/O, or dependencies on platform code. Platforms implement injected ports.
  Execution context and lifetimes belong to callers.
- UI-to-platform dependencies use designed adapters with explicit target-level
  visibility, UI-owned channels, or backend ports wired by composition. Use
  the groups in `//bazel/layers` for visibility; never widen a layer to bypass a
  guard. Transitional ratchet entries only shrink.
- Warnings are errors in first-party code. Fix their cause; never commit
  warning opt-outs, pragma suppressions, or `-Wno-*` flags. See
  [ADR 0018](docs/adr/0018-treat-first-party-warnings-as-errors.md).
- Backend exceptions never cross ports. Check `Result`/`Status` with
  `.has_value()`; adding an `ErrorKind` requires an ADR. Pure logic uses portable
  byte types, with explicit Qt conversions at desktop boundaries; see
  [coding conventions](docs/coding-style.md).
- Tests and mocks are package-owned. Every C++ suite uses GoogleTest. Follow
  the [testing conventions](docs/coding-style.md#tests) and
  [Gazelle ownership rules](docs/adr/0017-generate-bazel-targets-with-gazelle.md);
  regenerate managed BUILD files and review the output. Platform differences
  belong in selected sources; unavoidable compiler guards use `_WIN32`.
- Consult the [qualification matrix](docs/flash-qualification-matrix.md) and
  applicable [checklist](docs/checklists/) before hardware-facing work. Never
  relax an address-window guard or claim qualification without recorded evidence.

## Keep documentation current

Update the document that owns the changed decision or contract, and link to it
from other documents. Avoid copied inventories and status snapshots. Report
disagreement between implementation and intended contract before reconciling it.
Completed plans/specs leave the checkout after enduring knowledge is extracted
and their original contents are preserved in delivered Git history. Use human-
readable Markdown links so the existing link checker can validate references.

## Git workflow

Use a feature branch and pull request; never commit directly to `master`.
Preserve unrelated working-tree changes and stage named paths. Push when
authorized. Keep history-preservation and deletion changes separate when a
squash merge would otherwise discard the original document contents.
