# Extension load smoke test

Checks that a real `clang-tidy` host loads the extension and runs its checks.
[`smoke.py`](smoke.py) analyzes [one deliberate violation](smoke.cpp) without the
application compilation database or Qt. It requires exactly one
`fastecu-template-typename` error at the `class` token, with the existing
`typename` fix-it, and the expected failure exit status from clang-tidy. A
missing extension, an ignored load request or a silent no-checks run cannot pass.

Use absolute paths because Bazel launches Python binaries from their runfiles:

```sh
LLVM_CONFIG=/absolute/llvm/bin/llvm-config \
  bazel build --config=release //tools/clang-tidy/plugin:fastecu_tidy
bazel run --config=release //tools/clang-tidy/plugin:load_smoke -- \
  --clang-tidy /absolute/llvm/bin/clang-tidy \
  --plugin /absolute/checkout/bazel-bin/tools/clang-tidy/plugin/libfastecu_tidy.dylib
```

On Linux the extension suffix is `.so`. A linked analysis host can run the same
smoke without `--plugin`; the check must already be registered in that host.
