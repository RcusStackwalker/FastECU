# Step 5 tail wave 2 final fixes

## Final review fix wave

Base: `95b7340d` (`docs: close out step 5 tail wave 2 implementation`)

### Finding map

1. **MC68 upload and boot-init no-frame semantics**
   - Split the exchange helper into an `OptionalBytes`-preserving primitive and a framed-response wrapper.
   - Boot-init no-frame or mismatched data now follows the legacy kernel-ID fallback; upload succeeds only for the real no-frame outcome. A present empty frame remains distinct and is rejected.
   - Regressions use `queue_no_frame()` for stock/EcuTek upload, boot-init fallback, and a separate present-empty-frame negative case.
   - Files: `subaru_denso_mc68hc16y5_02_executor.cpp`, `subaru_denso_mc68hc16y5_02_executor_test.cpp`.

2. **MC68 packed read geometry and exact pages**
   - Read walks the selected device's flash blocks in logical order until its declared packed ROM size is complete. Stock wire addresses therefore jump from `0x1fc00` to `0x28000`, omit the physical `0x20000..0x27fff` hole, and return exactly `0x28000` bytes.
   - Page responses must be exactly `0x400 + 6` bytes. Short/truncated pages fail with `BadResponse`.
   - The known TPU table anomaly remains usable by stopping after its declared `0x1000` packed ROM size rather than reading all anomalous table blocks.
   - Files: MC68 executor/header/tests.

3. **Desktop MC68 physical/padded image normalization**
   - The MC workflow now preserves an already-packed image of `flashdev_t::romsize` and converts an exact physical-extent image by concatenating actual flash blocks.
   - Stock `FullRomData` of `0x30000` bytes becomes the packed `0x28000` plan image; the calibration service's inserted `0x8000` hole is excluded.
   - Tests cover physical Write integration, packed TestWrite preservation, and packed-read-shaped bytes through `apply_flash_method_padding()` and back through workflow normalization byte-for-byte.
   - Files: `flash_workflow.cpp`, `flash_workflow_test.cpp`, desktop flash `BUILD.bazel`.

4. **Fail-closed validation**
   - Generic validation exhaustively maps every `FlashFamily` to its one `FamilyPlan` alternative and required transport (all 49 family/variant combinations tested).
   - MC protocol/MCU identity is exact: stock/EcuTek use `MC68HC16Y5`; TPU uses `MC68HC16Y5_TPU`. SH bare/EcuTek use exactly `SH7055`.
   - MC validates canonical wire fields, empty erase/confirmation sets, exact model transfer/image geometry, canonical `0x20000` kernel address, 16-byte padding, model-region fit, and 24-bit wire length.
   - SH validates the canonical `0xffff6004` logical entry and the actual wire layout: the four-byte envelope starts at `0xffff6000`, and envelope plus four-byte-padded payload must fit the model's `0x6000` RAM region.
   - MC executor invokes its family validator before transport configuration/open/read/write and uses `get_if`; malformed plans have zero I/O.
   - Files: `flash_validation.cpp`/tests, both plan implementations/tests, MC executor/tests.

5. **SH Unix OpenPort2 post-upload delay**
   - Added a Qt-free K-Line transport capability, with desktop and scripted implementations.
   - Desktop reports the capability only on Unix when `SerialPortActions::get_use_openport2_adapter()` is true.
   - SH upload uses `IClock::sleep(5000, cancellation)` after the raw upload write and before the response read. Tests cover successful timing/order, cancellation before response I/O, the desktop adapter flag, and unchanged default adapters.
   - Files: `flash_executor.h`, scripted transport, desktop K-Line transport/tests, SH executor/tests.

### Minor findings

- SH `_ecutek` kernel resolution now looks up the protocol entry directly and no longer requires an unrelated car-model reference; a catalog fixture intentionally omits that row.
- `FlashPlan::experimental_family_id()` covers both wave-2 families.
- SH Write and TestWrite tests reject both undersized and oversized images.
- A kernel-alive SH Read regression returns bytes with no fabricated `rom_id` and performs no ECU-ID exchange.
- Corrected stale `_02_tpu` reachability, builder-interface, and task-reference text in the design/plan.
- MC diagnostics/progress were not expanded, per final-review scope.

### TDD evidence

- MC executor RED: real no-frame upload/boot fixtures failed as `Timeout`; packed-read address expectations and short-page rejection failed. GREEN: focused executor target passed.
- Workflow RED: physical/calibration-padded `0x30000` images failed preflight with `ROM file must be exactly 0x28000 bytes`. GREEN: packed, physical, and calibration round-trip cases passed.
- Generic validation RED: same-transport cross-family variants were accepted and both wave-2 IDs returned `Unknown`. GREEN: exhaustive matrix and IDs passed.
- MC plan/executor RED: known wrong MCUs, altered wire fields/geometry/kernel bounds were accepted, and malformed plans performed transport I/O. GREEN: plan and executor targets passed with zero-I/O assertions.
- SH plan RED: known wrong MCUs, shifted kernel entries, and the envelope-overrun boundary were accepted. GREEN: exact identity/boundary suite passed.
- OpenPort2 RED: desktop capability stayed false and SH timing omitted `5000`. GREEN: desktop and SH executor targets passed, including cancellation-before-read.
- TPU read RED: traversal continued into anomalous extra blocks. GREEN: declared packed-size regression passed.

### Verification

- Final uncached `bazel test --config=release --nocache_test_results` over both MC/SH plan and executor tests, `flash_validation_test`, workflow, calibration service/adapter, file-actions parsing, desktop K-Line transport, `portable_closure`, and `legacy_flash_drain`: **12/12 targets passed** (exit 0).
- `bazel build --config=release //:fastecu`: **passed** (exit 0). The build emitted only existing warnings in unrelated desktop files.
- `git diff --check`: **passed**.
- Commit hooks (`clang-format`, `buildifier`, `buildifier-lint`, whitespace/EOF/BOM, header pragma, and link checks): **passed**.
- Independent read-only final diff review: **no Critical, Important, or Minor findings**.

### Files and commits

- Production: MC/SH plan and executor files; generic flash validation/plan ID; K-Line transport interface/scripted/desktop adapters; desktop workflow and BUILD dependency cleanup.
- Tests: MC/SH plan/executor suites, generic validation, workflow/calibration round trip, desktop K-Line adapter.
- Docs: implementation plan and design corrections; this report.
- Implementation commit: `359d2025` (`fix(flash): address wave 2 final review`).

### Remaining concerns

- No ECU or OpenPort2 hardware qualification was performed; hardware status remains experimental/unqualified. The adapter-specific wait is covered through deterministic transport/clock seams and the real desktop adapter flag.
- `MC68HC16Y5_TPU` still has the pre-existing shared-table inconsistency (declared `0x1000` ROM with four `0x1000` blocks). The executor deliberately honors declared packed ROM size; the table itself was not changed.
- Existing unrelated compiler warnings remain in the desktop release build.
