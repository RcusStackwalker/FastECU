# FileActions Debug Helper Refactor

## Goal

Keep the debug-only XML traversals in `file_actions.cpp` syntactically checked while ensuring they compile to no runtime work unless file-action debugging is explicitly enabled.

## Design

- Define `static constexpr auto kDebugFileActions = false;` in the file's anonymous namespace.
- Move each traversal whose only purpose is emitting diagnostic `LOG_D` messages into a focused private free helper in that namespace.
- Begin each helper with `if constexpr (!kDebugFileActions) { return; }`. The remaining helper body stays well-formed and compiler-checked, while an optimizing build can eliminate the disabled call and body.
- Pass a logging callback into the helpers so anonymous-namespace functions do not need access to `FileActions` signal internals.
- Replace the inline debug-only traversals with helper calls. Do not alter the XML parsing or populated `LogValuesStructure` fields.

## Verification

- Build the target containing `file_actions.cpp` to prove the restored logging statements and helpers are syntactically valid.
- Run the focused file-actions parsing tests to confirm normal parsing behavior is unchanged and disabled debug traversals emit nothing.
