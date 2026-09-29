# 6n-3 Desktop Byte Boundary Design

## Intent and scope

Finish step 6 desktop closure by placing the remaining Qt byte-conversion
helper at the desktop boundary. Preserve its public functions, namespace,
byte output, and explicit calls from existing desktop consumers. This is an
ownership relocation, not a conversion of Qt-owned buffers or a redesign of
protocol byte handling. Step 7 Android work follows desktop closure.

## Current state

`src/algorithms/protocol/qt_compat:qt_compat` exports the header-only
`qt_bytes.h`. It bridges `QByteArray` and `bytes` views or owned buffers,
provides Qt-container append and write overloads, and formats a Qt byte array
as a `QString`. Desktop platform adapters, two UI packages, and an integration
test include it. Its co-located test mixes portable byte tests and Qt helper
tests. The package is the last algorithms-side entry in `qt_layer`.

## Selected boundary

Move the helper to `src/platform/desktop/common/bytes/qt_bytes.h` in a
dedicated header-only `:qt_bytes` target. Keep the `bytes` namespace and all
function signatures and bodies unchanged. The target depends on Qt Core and
the portable `src/algorithms/protocol` target. It is a deliberately designed
desktop adapter: target-level visibility permits desktop platform packages,
the existing root integration test, and the two direct UI consumer packages
`src/ui/desktop` and `src/ui/desktop/biu`. Do not grant backend or algorithms
access. Use the repository's layer groups for broad layer grants and explicit
package grants for the two UI consumers.

No new portable port or byte representation is needed. Portable code keeps
using `bytes::Byte`, `bytes::Bytes`, and `bytes::ByteView`; Qt-owned code makes
the same explicit conversions at its existing call sites. The target exposes
no serial facade header or other platform implementation detail.

## Migration

Move `qt_bytes.h` and its existing test into the new package. Preserve the
test's portable assertions while moving it, so the relocation does not narrow
coverage. Update active includes and Bazel dependencies in desktop platform,
UI, and the root integration test. Remove the old package and its
`qt_layer` entry only after consumers resolve to the new target. Do not add a
forwarding header or alias at the old path.

Update current guidance in `CLAUDE.md`, the modularization plan, design notes,
and any current build-graph guidance that names the old location. Historical
ADRs retain their original decision text; if a current-location note is
needed, make it clearly subsequent to that decision. Documentation fixture
examples are not production dependencies and should be changed only when they
claim to describe the current package graph.

## Compatibility and failure modes

A missed include or dependency will fail at build analysis or compilation.
An accidental edit to a conversion overload could change byte order, copy
behavior, or the trailing-space hex format. Compare the moved header with its
prior contents and run the moved conversion tests. Preserve view lifetimes:
`view` and `mutableView` remain non-owning views into the caller's
`QByteArray`; `fromQByteArray` and `toQByteArray` still copy into owned output.
The migration adds no exception handling, I/O, threading, or device behavior.

## Verification and exit criteria

- Confirm the helper's function bodies are unchanged and no active include or
  Bazel dependency names `src/algorithms/protocol/qt_compat`.
- Run Gazelle consistency, the moved conversion test, and affected desktop
  package and integration tests.
- Build the desktop application, `//:portable_closure`, and release `//...`;
  run release `//...`, formatting/lint, and changed-file static analysis.
- Confirm `qt_layer` contains no algorithms or backend exception and no
  Qt-typed legacy package remains under those trees. Preserve the serial
  facade's UI header boundary.
- Mark step 6 structurally complete only after these gates pass. Record
  platform CI, packaging, and hardware qualification separately; this move
  itself adds no hardware qualification.
