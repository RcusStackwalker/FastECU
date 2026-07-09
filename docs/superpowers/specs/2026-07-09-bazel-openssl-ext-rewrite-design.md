# Rewrite `bazel/openssl_ext.bzl` for robust external OpenSSL support

## Problem

`bazel/openssl_ext.bzl` locates a host-installed OpenSSL and exposes it to
Bazel as `@openssl_macos//:openssl` / `@openssl_windows//:openssl`. Linux
bypasses the extension entirely and links ambient `-lcrypto` directly in the
top-level `BUILD.bazel`.

The current implementation is broken on Windows right now: the top-of-branch
commit (`f5d2eba`) fails Bazel CI with

```
LINK : fatal error LNK1181: cannot open input file 'lib\VC\x64\MD\libcrypto.lib'
```

Root cause: the rule discovers the OpenSSL library file, then embeds the
*host filesystem path* to it as hand-formatted text inside a generated
`linkopts = [...]` list in a templated `BUILD.bazel` file. This is fragile in
two independent ways:

1. **String escaping.** Paths containing spaces/backslashes (`C:/Program
   Files/OpenSSL/...`) need careful quoting inside a Python-style string
   embedded in another string. The three most recent commits on this branch
   (`ci: linking`, `ci: show link params`, `ci: try fix with relative path`)
   are visibly fighting this class of bug.
2. **Path resolution.** A relative path (`lib\VC\x64\MD\libcrypto.lib`) only
   resolves if the linker's working directory happens to be the OpenSSL
   root — it isn't, which is the direct cause of the current `LNK1181`.

There's also a structural inconsistency: macOS/Windows go through a
repository-rule mechanism that silently falls back to an empty stub
`cc_library` when OpenSSL isn't found (deferring the real failure to a
confusing later "undefined reference" link error), while Linux has a
completely separate, un-overridable ambient-linking path.

## Goals

- Fix the Windows link failure by construction, not by patching the
  symptom — eliminate embedding host paths into generated Starlark text.
- One consistent mechanism across macOS, Windows, and Linux, with a single
  failure mode when OpenSSL can't be located.
- Fail the Bazel analysis phase loudly and immediately when OpenSSL is
  required but missing, naming what was searched and how to fix it.
- Keep Linux's existing "just works with `apt install libssl-dev`" ergonomics
  — don't force path discovery where the system linker already handles it.
- Windows output stays self-contained (no separate OpenSSL install required
  to run the built exe/tests) without introducing CRT-mixing risk.

## Non-goals

- **No static linking of OpenSSL on Windows.** The Windows OpenSSL installer
  ships a true static archive (`lib/VC/x64/MT/libcrypto.lib`, built against
  the static CRT), but the project's Qt is a prebuilt dependency fetched by
  `rules_qt` and built against the dynamic CRT (`/MD`). Linking the `/MT`
  archive in would require `/NODEFAULTLIB:LIBCMT` to suppress the resulting
  CRT conflict — technically workable since there's no DLL boundary at
  allocation time, but a real footgun class we're deliberately avoiding.
  Instead: keep the existing dynamic import-lib (`MD`) linking, and have
  Bazel automatically stage `libcrypto-3-x64.dll` next to the output binary
  (see below) so the result is self-contained without touching CRT linkage.
- **No cleanup of `release.yml`/`pr.yml`'s manual
  `copy "$OPENSSL_ROOT/bin/$OPENSSL_CRYPTO_DLL" dist\` step.** That step
  belongs to the qmake release path, not the Bazel path this design touches.
  Explicitly out of scope for this round.
- **No pkg-config integration, no vendoring/building OpenSSL from source.**
  This remains "find an already-installed OpenSSL," same as today.

## Design

### One unified repository, not three

Replace the two platform-named repos (`openssl_macos`, `openssl_windows`,
each running the *same* implementation gated only by `rctx.os.name` at fetch
time) and Linux's separate ambient-linking `select()` branch with a single
`openssl` repository. `fastecu_core_common` in the top-level `BUILD.bazel`
depends unconditionally on `@openssl//:openssl` — the OpenSSL arm of that
target's `select({...})` goes away entirely.

`MODULE.bazel` changes from:

```python
openssl = use_extension("//bazel:openssl_ext.bzl", "openssl")
use_repo(openssl, "openssl_macos", "openssl_windows")
```

to:

```python
openssl = use_extension("//bazel:openssl_ext.bzl", "openssl")
use_repo(openssl, "openssl")
```

### Fixing the root cause: symlink to a fixed name, then `cc_import`

The rule still searches the same candidate roots it does today
(`OPENSSL_ROOT` env var first, then Homebrew paths on macOS, then common
Windows install paths), and still searches the same per-platform library
subpath candidates (e.g. Windows' `VC/x64/MD/libcrypto.lib`,
`VC/x64/MT/libcrypto.lib`, flat `lib/libcrypto.lib`). What changes is what
happens *after* a candidate is found:

1. `rctx.symlink()` the discovered header directory to a fixed repo-relative
   path: `include/`.
2. `rctx.symlink()` the *specific discovered library file* (whichever
   candidate matched) to a fixed repo-relative name, e.g. `lib/libcrypto.lib`
   on Windows, `lib/libcrypto.dylib` on macOS, `lib/libcrypto.so` on Linux
   (only when `OPENSSL_ROOT` is set — see below). All the "which nested
   subdirectory / which variant" logic is absorbed here; nothing downstream
   ever sees the original host path.
3. Generate `BUILD.bazel` from a **static template** — no interpolated host
   paths, only the fixed relative filename chosen in step 2 (which never
   contains spaces or backslashes, so there's nothing to escape) — using
   `cc_import` for the prebuilt library, wrapped in a `cc_library` that
   supplies `hdrs`/`includes`:

   ```python
   cc_import(
       name = "crypto_import",
       interface_library = "lib/libcrypto.lib",  # Windows: import lib
       # or: static_library = "lib/libcrypto.a"   (if ever needed)
       # or: shared_library = "lib/libcrypto.so"  (Linux w/ OPENSSL_ROOT)
   )

   cc_library(
       name = "openssl",
       hdrs = glob(["include/openssl/**/*.h"]),
       includes = ["include"],
       deps = [":crypto_import"],
       visibility = ["//visibility:public"],
   )
   ```

   `cc_import` treats the library file as a tracked build input resolved by
   Bazel relative to the execroot/sandbox, not as opaque linker-flag text —
   this is what actually fixes the path-resolution bug class, independent of
   the escaping cleanup.

### Per-platform behavior

- **macOS:** always resolve a root (`OPENSSL_ROOT` env var, else
  `/opt/homebrew/opt/openssl@3`, else `/usr/local/opt/openssl@3`). Symlink
  `include/` and the discovered `libcrypto.dylib`. `fail()` if no candidate
  root exists.
- **Windows:** always resolve a root (`OPENSSL_ROOT` env var — set by CI's
  choco install step — else the installer's default paths as a convenience
  fallback for local dev). Symlink `include/` and the discovered
  `libcrypto.lib` (import lib, `MD` variant preferred, matching today's
  search order). `fail()` if no root or no matching lib subpath is found.
  Additionally, discover `bin/libcrypto-*.dll` under the same root (preferring
  the highest SONAME, same tie-break already used in `pr.yml`'s Windows
  OpenSSL install step), symlink it to a fixed `bin/libcrypto.dll`, and add it
  to the `cc_import`/`cc_library` as a runtime `data` dependency so Bazel
  automatically stages it next to any binary (`fastecu.exe`, test binaries)
  that transitively depends on `@openssl//:openssl`. No manual copy step
  needed for Bazel-built outputs.
- **Linux:**
  - If `OPENSSL_ROOT` is set (custom/non-system OpenSSL): resolve it the same
    way as macOS/Windows — symlink `include/` and the discovered
    `libcrypto.so`, wire up via `cc_import`.
  - If `OPENSSL_ROOT` is unset (the common case — `apt install libssl-dev`
    puts headers/libs on the compiler and linker's default system search
    paths already): skip path discovery entirely and emit a `cc_library`
    with `linkopts = ["-lcrypto"]` and no `hdrs`/`includes` overrides. This
    deliberately avoids reverse-engineering Debian's multiarch lib
    directories (`x86_64-linux-gnu` vs `aarch64-linux-gnu`) when the system
    linker already resolves it for free. No `fail()` in this branch — an
    unresolved `-lcrypto` still fails the build, just at link time with a
    standard "cannot find -lcrypto," which is an acceptable, well-understood
    failure mode for the ambient-system case (unlike today's *silent* stub,
    this isn't hiding a resolvable-but-undetected install).

### Fail fast

Whenever the rule *does* attempt path discovery (macOS and Windows always;
Linux when `OPENSSL_ROOT` is set) and finds no matching candidate, the
repository rule calls `fail()` naming the paths it searched and suggesting
`OPENSSL_ROOT`. This replaces today's silent empty-stub `cc_library` +
deferred "undefined reference" link error with an immediate, actionable
analysis-phase failure.

### Test/script updates

`scripts/check-bazel-openssl-windows.py` (run as the `bazel_openssl_windows`
`py_test`) currently asserts `MODULE.bazel` exposes `openssl_windows` and
`BUILD.bazel`'s `fastecu_core_common` selects `@openssl_windows//:openssl`.
Both assertions describe repo names that no longer exist after this rewrite.
Rename the script to `check-bazel-openssl.py` (test target
`bazel_openssl_wiring`) and update its checks to the new invariants:

- `MODULE.bazel` has `use_repo(openssl, "openssl")`.
- `BUILD.bazel`'s `fastecu_core_common` depends on `@openssl//:openssl`
  unconditionally (no `select()` needed for it anymore).
- The Windows CI job in `.github/workflows/pr.yml` still exports
  `OPENSSL_ROOT` (unchanged check, just kept).

No other CI workflow changes: Windows already sets `OPENSSL_ROOT` via choco,
macOS already runs `brew install openssl@3`, Linux already runs
`apt-get install libssl-dev`.

## Testing / validation

- `bazel build -k --config=release //:fastecu //tests/...` and
  `bazel test -k --config=release //tests/... //:qmake_bazel_sync
  //:bazel_openssl_wiring` green on all three CI matrix legs
  (`windows-latest`, `macos-latest`, `ubuntu-latest`), confirming the Windows
  `LNK1181` regression is actually fixed.
- Manually verify (or add a smoke check) that a locally-run Windows test
  binary finds `libcrypto.dll` next to it without needing `OPENSSL_ROOT/bin`
  on `PATH`, confirming the DLL auto-bundling works.
- Delete `OPENSSL_ROOT` locally (or point it at a bogus path) and confirm
  `bazel build` fails at analysis time with the new `fail()` message rather
  than a link-time error, on both macOS and Windows.

## Open follow-ups (not blocking this design)

- Cleaning up the now-redundant manual DLL copy step in `release.yml`/
  `pr.yml`'s qmake-based static packaging path, once/if that path is folded
  into Bazel per ADR 0001.
- Extending the same fixed-name-symlink + `cc_import` pattern to other
  prebuilt third-party libs if any are added later.
