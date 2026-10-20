# clang-tidy tooling

Build-time tooling for FastECU's static analysis, kept apart from the
application:

- [`plugin/`](plugin/) holds the `fastecu-*` clang-tidy checks, their lit
  fixtures and a load smoke test.
- [`scripts/`](scripts/) holds the runner that drives clang-tidy over the
  repository, plus its tests.

Usage is documented in the [tooling guide](../../docs/tooling.md#clang-tidy).

## License

Everything under `tools/clang-tidy/` is licensed under the Apache License v2.0
with LLVM Exceptions (see [`LICENSE`](LICENSE)), not the GPLv3 that covers the
rest of the repository. The plugin builds against LLVM and clang, which use the
same license. It is a development tool: it is not linked into or distributed
with the FastECU application.

Two things to keep in mind:

- The lit fixtures in `plugin/test/` use FastECU headers such as
  `src/backend/ports/result.h` as analysis input. They are only read by
  clang-tidy, and the fixtures themselves contain no FastECU code, but they can
  only be run inside a FastECU checkout.
- Commits made before this directory carried its own license were published
  with the repository under the GPLv3, and that grant stays in place for those
  revisions. The Apache License applies from the commit that added `LICENSE`
  onwards.
