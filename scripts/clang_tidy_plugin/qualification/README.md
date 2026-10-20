# LLVM packaging qualification

The exact macOS/Linux shared-host smoke and the Windows linked-host workflow
have passed.

## Smoke contract

The [Bazel-owned smoke runner](smoke.py) analyzes [one deliberate violation](smoke.cpp)
without the application compilation database or Qt. It requires exactly one
`fastecu-template-typename` error at the `class` token, with the existing
`typename` fix-it, and the expected failure exit status from clang-tidy. Thus a
missing extension, an ignored load request or a silent no-checks run cannot pass.
Running without the extension failed with zero diagnostics, as expected.

Use absolute paths because Bazel launches Python binaries from their runfiles:

```sh
LLVM_CONFIG=/absolute/llvm/bin/llvm-config \
  bazel build --config=release //scripts/clang_tidy_plugin:fastecu_tidy
bazel run --config=release //scripts/clang_tidy_plugin:qualification_smoke -- \
  --clang-tidy /absolute/llvm/bin/clang-tidy \
  --plugin /absolute/checkout/bazel-bin/scripts/clang_tidy_plugin/libfastecu_tidy.dylib
```

On Linux the extension suffix is `.so`. A linked analysis host can run the same
smoke without `--plugin`; the check must already be registered in that host.

## Evidence on 2026-10-20

The selected release is
[LLVM 23.1.2](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.2),
matching CI. Downloaded Zstandard archives were checked against the release
asset SHA-256 metadata:

| Archive | SHA-256 |
| --- | --- |
| `LLVM-23.1.2-macOS-ARM64.tar.zst` | `3da0e91b5dfe3a5ec795ad2be79b3f5e6f28c8b23edcd3847fad7742b25e0507` |
| `LLVM-23.1.2-Linux-X64.tar.zst` | `6382de1c1a210ce5a5cc49d18bc8444d137742e7cbf9b19f4ae602bb1ab52534` |
| `clang+llvm-23.1.2-x86_64-pc-windows-msvc.tar.zst` | `ceaee048142fece144752c6f6431cb0905a7a6160f78ab8cf5cf0b6216f99418` |
| `LLVM-23.1.2-Linux-X64.tar.xz` (CI) | `b5ed9675149cc837c282e9b6962c276c9fa62863d5b2f91537b60848552995b7` |
| `clang+llvm-23.1.2-x86_64-pc-windows-msvc.tar.xz` (CI) | `8fb91cdc44fcbbdcf6b3ffd0a1f9859abd14a3c3aae4423c2b6d4a4f90bf0095` |

### macOS ARM64

The exact official 23.1.2 `llvm-config` and `clang-tidy` reported 23.1.2. The
existing extension built through Bazel against that installation, and the smoke
passed with the diagnostic and replacement above. A separate Homebrew 23.1.3
build/load also passed, but does not qualify the pinned release.

`LLVM_CONFIG=/nonexistent/fastecu-qualification-llvm-config bazel build
--nobuild --config=release //:fastecu` also passed target analysis. Application
target loading therefore does not require LLVM discovery in this checkout.

### Linux x86-64

The checksum-verified official distribution built and passed the smoke in an
Ubuntu 24.04 x86-64 container using Bazel 9.1.1 and the selected LLVM Clang
compiler. The shared extension emitted the required diagnostic and fix-it.
The application build remains independently qualified by platform CI.

### Windows x86-64

Archive inspection found the clang-tidy headers, `clangTidy.lib`,
`clangTidyMain.lib`, check-module libraries, and Clang/LLVM static libraries.
`llvm-readobj --coff-exports bin/clang-tidy.exe` reported no exports. Therefore
the existing extension, which resolves symbols from its loading executable,
cannot use this executable's clang-tidy symbols. This is evidence about the
pinned release executable, not a universal Windows prohibition.

The agreed linked-host fallback has additional dependencies absent from the
archive:

- `lib/cmake/llvm/LLVMExports.cmake` records `ZLIB::ZLIB` and
  `zstd::libzstd_static` as dependencies of `LLVMSupport`.
- `llvm-nm --undefined-only lib/LLVMSupport.lib` confirms unresolved
  `compress2`, `compressBound`, `uncompress`, and `ZSTD_*` symbols.
- The archive manifest contains no zlib/zstd library artifacts.
- `llvm-readobj --coff-directives lib/clangTidyMain.lib` records
  `RuntimeLibrary=MT_StaticRelease`, `_ITERATOR_DEBUG_LEVEL=0` and
  `LLVM_ENABLE_ABI_BREAKING_CHECKS=0`. The host and extra libraries must use
  compatible settings; a matching release number alone is insufficient.

The user approved supplying zlib/zstd as Bazel-built static dependencies with
compatible runtime settings. The host now builds with the selected clang-cl;
LLVM dependency headers use `/imsvc` while first-party warnings remain errors.
Bazel generates the two missing force-linker configuration switches from the
optional module libraries installed in the archive.

[CI run 39394823557](https://github.com/RcusStackwalker/FastECU/actions/runs/39394823557/job/118535102310)
built and ran the linked host, validated all 16 registered checks, reported zero
findings in the 41-unit Windows-exclusive scope, and passed all 37 real-dependency
fixtures. The template fixture verifies the original diagnostic location,
severity, replacement text and replacement range. This closes the Windows
packaging prerequisite without building LLVM from source.
