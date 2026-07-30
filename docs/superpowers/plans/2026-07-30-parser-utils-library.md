# Parser Utilities Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the 510-line header-only parser utility collection into a dedicated portable C++ library.

**Architecture:** `parser_utils.h` exposes declarations and retains only the callback-dependent `populate_axes` template. `parser_utils.cpp` owns every ordinary implementation. Both format parsers link a shared `:parser_utils` target.

**Tech Stack:** C++23, pugixml, Bazel, GoogleTest

## Global Constraints

- Preserve all parser behavior, errors, optional-field semantics, and public APIs.
- Keep `//src/backend/definition:parser_utils` in the portable dependency closure.
- Keep new-code duplication below SonarCloud's 3% threshold.

---

### Task 1: Extract the parser utilities library

**Files:**
- Create: `src/backend/definition/parser_utils.cpp`
- Modify: `src/backend/definition/parser_utils.h`
- Modify: `src/backend/definition/BUILD.bazel`
- Modify: `BUILD.bazel`
- Modify: `scripts/check-portable-closure.py`
- Test: `src/backend/definition/romraider_parser_test.cpp`
- Test: `src/backend/definition/ecuflash_parser_test.cpp`

**Interfaces:**
- Consumes: shared definition model types, `Result`, `Status`, and pugixml node types.
- Produces: the existing parser utility function signatures through `:parser_utils`; retains `template <typename ParseAxis> Status populate_axes(...)` in the header.

- [ ] **Step 1: Establish the green behavioral baseline**

Run:

```bash
bazel test //src/backend/definition:all //:portable_closure
```

Expected: all definition and portability tests pass.

- [ ] **Step 2: Replace inline definitions with declarations**

Keep the existing signatures in `parser_utils.h` without `inline`. Retain the
complete `populate_axes` template because its callable type is instantiated by
each parser.

- [ ] **Step 3: Move ordinary implementations**

Create `parser_utils.cpp`, include `parser_utils.h`, and move the unchanged
bodies of:

```text
trim_copy
detail_prefix
invalid
child_text
identity_element
required_child_text
definition_id_for_rom
parse_metadata
parse_root
parse_hex_unsigned
value_or_empty
selection_name
optional_hex_element
optional_hex_attribute
optional_address
dimension_attribute
strict_boolean_attribute
populate_common_axis_attributes
apply_scaling_to_axis
populate_common_map_attributes
populate_optional_dimension
populate_optional_boolean
```

- [ ] **Step 4: Add the Bazel library boundary**

Add:

```starlark
cc_library(
    name = "parser_utils",
    srcs = ["parser_utils.cpp"],
    hdrs = ["parser_utils.h"],
    deps = [
        ":definition_model",
        "//src/backend/ports",
        "@pugixml",
    ],
)
```

Remove `parser_utils.h` from both parser `srcs` lists and add
`:parser_utils` to both parser `deps` lists. Add `:parser_utils` to the root
portable genquery/scope and to `PORTABLE_ROOTS` in
`scripts/check-portable-closure.py`.

- [ ] **Step 5: Verify behavior and build boundaries**

Run:

```bash
bazel test //src/backend/definition:all //:portable_closure
prek run --all-files
git diff --check
```

Expected: all commands pass.

- [ ] **Step 6: Commit and publish**

```bash
git add BUILD.bazel scripts/check-portable-closure.py src/backend/definition/BUILD.bazel src/backend/definition/parser_utils.h src/backend/definition/parser_utils.cpp
git commit -m "refactor: extract parser utilities library"
git push
```

Monitor PR #105 and confirm SonarCloud reports a passing quality gate with
new-code duplication below 3%.
