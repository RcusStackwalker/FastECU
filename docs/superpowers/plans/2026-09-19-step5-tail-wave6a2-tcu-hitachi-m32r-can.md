<!-- docs/superpowers/plans/2026-09-19-step5-tail-wave6a2-tcu-hitachi-m32r-can.md -->
# Wave 6a-2 — Subaru TCU Hitachi M32R CAN — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate `FlashTcuSubaruHitachiM32rCanOperation` (1,036 lines, legacy Qt) to a portable `FlashPlan` + `ICanFlashExecutor` pair, removing one entry from `//:legacy_flash_drain`.

**Architecture:** ISO-15765 at 500 kbit/s, request `0x7E1` / response `0x7E9`, driving the TCU's **resident on-board kernel** — no kernel image is uploaded. Connect authenticates (SecurityAccess `0x27`) and jumps to the kernel (`0x10 0x02`); reads use a `0x34` window setup then a `0xB7` page-dump loop; writes erase the whole flash (`0x31 02 01 FF FF FF FF`) then reflash blocks 3–10 in 128-byte `0xB6` frames. Payloads are SSM-encrypted. The plan builder is hand-written, not `single_window_plan`.

**Tech Stack:** C++23, Bazel, GoogleTest (`fastecu_portable_gtest`), QtTest for the desktop workflow suite. Portable backend code: no Qt, no threads, no filesystem.

**Spec:** [Step 5 Tail Wave 6 — Nine Singletons](../specs/2026-09-19-step5-tail-wave6-singletons-design.md)

**Reference implementation:** wave 6a-1, merged in PR #347, is the proven template for every file shape here — `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_{types.h,plan.h,plan.cpp,executor.h,executor.cpp,plan_test.cpp,executor_test.cpp}`. Copy its structure, its comment density, and its test helpers' shape. **This plan deliberately does not re-transcribe code that already exists on `master`** — wave 6a-1's plan did, and its transcriptions carried thirteen defects. Where this plan says "mirror X", open X and mirror it. Where it gives exact wire bytes or constants, those are the load-bearing values and must be exact.

## Global Constraints

- C++23. Backend operations return `fastecu::Result<T>` / `Status`, checked with `.has_value()` and **never** the implicit `operator bool`.
- **Exceptions never cross a port.** The `ErrorKind` set is closed — do not add a value.
- Pure protocol logic uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView`. `QByteArray` is a boundary type only.
- `//src/backend/flash/ecu` is a **portable** package: no Qt, no threads, no filesystem may become reachable from it.
- **Every new `cc_library` must be registered by name in `PORTABLE_PACKAGES`** (`bazel/portable_targets.bzl`). That file maps each package to a list of individual target names; an unregistered target is silently never swept by `//:portable_closure`. This was missed in wave 6a-1 and left three targets unguarded.
- **No entry may be added to any ratchet list.** They only shrink.
- Test matchers are `IsOk()`, `IsOkAnd(m)`, `IsErr(kind)`, `IsErrWith(kind, m)` from `src/backend/ports/testing/result_matchers.h`. There is no `IsErrorOfKind` and no bare `IsError`.
- `FakeClock` and `RecordingEventSink` are in namespace `fastecu`, not `fastecu::testing`.
- Integer literal suffixes are UPPERCASE (`128U`); shifts use `>> 16U` — `.clang-tidy` enables `readability-uppercase-literal-suffix` and `bugprone-signed-bitwise`, and `bazel run //:clang_tidy_report_changed` is a PR gate.
- Tests are package-owned and co-located.
- Work lands through a pull request; `prek` refuses commits on `master`. **Do not push or open a PR without explicit authorization.**
- Every commit message ends with the two attribution lines this repo uses.

## Deliberate Divergences From Legacy

Six, each with a reason. All must appear in the `docs/flash-qualification-matrix.md` row and the PR body. Everything not listed is byte- and timing-preserving.

1. **TestWrite is rejected as `Unsupported`.** `reflash_block` takes `test_write_arg` (legacy line 713) and **never reads it** — the only occurrences of the name in the whole file are the signature and the call site at line 686. Selecting "test write" therefore performs a real erase and a real flash write, identical to "write", differing only in a log line. There is no dry-run implementation to port. Rejecting it is the only honest option; silently making it destructive is not, and inventing a dry run we cannot verify against the kernel is worse.
2. **The read window targets the clamp's evident intent, not the underflowed address.** `read_mem` computes `start_addr - 0x00100000`, which underflows `uint32_t` to `0xFFF00000` and bypasses its own `if (start_addr < 0x8000)` floor. The consequence is arithmetic, not stylistic: `willget` becomes `0x80000`, the loop accumulates `0x80000` dumped bytes, and the trailing zero-pad prepends another `0x8000` — producing a **0x88000-byte image for a 0x80000 ROM**. The live path cannot yield a valid ROM, so there is no working behavior to preserve. The port dumps `0x78000` bytes from `0x8000` and prepends `0x8000` zeros for exactly `0x80000`. This matches the wave-3 CVT sibling's decision (`subaru_tcu_cvt_hitachi_m32r_can_plan.cpp`), though that sibling reached it because its `read_mem` was unreachable dead code; here the path is live and simply broken.
3. **The kernel-alive re-check frame is built in bounds.** After `output` holds the six bytes `00 00 07 E1 10 02`, legacy does `output[6] = 0x02; output[7] = 0x01;` — two writes past the end. Under Qt 6.8.3 (`QT_VERSION` in `.github/workflows/pr.yml`) `QByteArray::operator[]` does not auto-extend; that was Qt 5's `QByteRef`. The port sends the well-formed eight-byte frame `00 00 07 E1 31 02 02 01`, identical to the first kernel-alive check at the top of `connect_bootloader`.
4. **The read zero-pad uses a sized buffer.** Legacy fills `QByteArray padBytes;` — default-constructed, empty — via `padBytes[i] = 0x00` for `i` in `0..0x7FFF`, the same out-of-bounds pattern as divergence 3. The port constructs `bytes::Bytes(0x8000, 0x00)`.
5. **Erase failure is fatal.** `erase_mem` reads with a 200 ms timeout after a 500 ms delay for a multi-second erase, calls `received.at(4)` with **no length guard**, and has its `return STATUS_ERROR` commented out — so an erase that fails, or returns nothing, is logged and ignored, and reflash then proceeds onto possibly-unerased flash. The port validates the response length before indexing and returns an error. This is the most dangerous defect in the family.
6. **Unreachable and unused legacy code is not ported:** `decrypt_payload`'s reversed-table twin is kept (it is used by the read path), but `init_flash_hitachi_can()` (commented out at legacy line 25) and the dead `hack_words`-style scaffolding are not.

**Two values this plan cannot verify from the repository**, both to be flagged `VERIFY:` in the matrix row and the PR body:
- The MCU binding. The wave-3 CVT sibling uses `M32R_512KB` and this family reads the same `flashdevices` table, so this plan uses `M32R_512KB`. Protocol-to-MCU mapping lives in EcuFlash-side definition files.
- Whether the TCU kernel tolerates the corrected read window. Divergence 2 changes the address on the wire from `F0 00 00` to `00 80 00`. That is the intent the clamp encodes, but it is unobserved on hardware.

## File Structure

**Create (portable backend, `src/backend/flash/ecu/`):**
- `subaru_tcu_hitachi_m32r_can_types.h` — the plan POD.
- `subaru_tcu_hitachi_m32r_can_plan.{h,cpp}` — hand-written builder and validator. No I/O.
- `subaru_tcu_hitachi_m32r_can_plan_test.cpp`
- `subaru_tcu_hitachi_m32r_can_executor.{h,cpp}` — an `ICanFlashExecutor`.
- `subaru_tcu_hitachi_m32r_can_executor_test.cpp`

**Modify:** `src/backend/flash/flash_types.h`; `src/backend/flash/ecu/BUILD.bazel`; `src/backend/flash/BUILD.bazel`; `bazel/portable_targets.bzl`; `src/platform/desktop/common/flash/flash_workflow.cpp` and its test and BUILD; `src/ui/desktop/mainwindow.{h,cpp}`; `src/ui/desktop/flash/tcu/BUILD.bazel`; `src/platform/desktop/common/flash/legacy/BUILD.bazel`; `scripts/check-legacy-flash-drain.py`; `docs/flash-qualification-matrix.md`.

**Delete:** `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_hitachi_m32r_can_operation.{h,cpp}`, `src/ui/desktop/flash/tcu/flash_tcu_subaru_hitachi_m32r_can.{h,cpp}`. After this the `legacy/tcu/` directory is empty — remove it.

## The Wire Protocol, Verified Against Legacy

Every frame below is the full payload handed to the transport, including the four-byte CAN-ID prefix the legacy builds by hand. Verified against `flash_tcu_subaru_hitachi_m32r_can_operation.cpp` at `3bafd6b5`-descendant `master`.

**Connect** (`connect_bootloader`, legacy 91–393):

| # | Request | Expected | On mismatch |
|---|---|---|---|
| 1 | `00 00 07 E1 31 02 02 01` | `[4]=0x71 [5]=0x02 [6]=0x02 [7]=0x03` | not fatal — means kernel not yet running; continue |
| 2 | `00 00 07 E0 AA` | `[4]=0xEA`; TCU ID = hex of bytes 8..12 | logged, **not fatal** |
| 3 | `00 00 07 E0 09 04` | `[4]=0x49 [5]=0x04`; CAL ID = bytes 7.. | logged, **not fatal** |
| 4 | `00 00 07 E0 10 03` | `[4]=0x50 [5]=0x03` | fatal |
| 5 | `00 00 07 E0 27 01` | `[4]=0x67 [5]=0x01`; seed = bytes 6..9 | fatal |
| 6 | `00 00 07 E0 27 02 <key4>` | `[4]=0x67 [5]=0x02` | fatal |
| 7 | `00 00 07 E1 10 02` | `[4]=0x50 [5]=0x02` | logged, **not fatal** (legacy's `return` is commented out) |
| 8 | `00 00 07 E1 31 02 02 01` | `[4]=0x71 [5]=0x02 [6]=0x02 [7]=0x03` | fatal |

Step 1 short-circuits: on a match, connect returns success immediately with `kernel_alive` set. Steps 2–6 each `delay(50)` between write and read; step 7 delays 200 ms.

**Read** (`read_mem`, legacy 395–620): window setup `00 00 07 E1 34 04 33 <addr24> <len24>` expecting `[4]=0x74 [5]=0x20 [6]=0x01 [7]=0x04`; then per page `00 00 07 E1 B7 <addr24>` expecting `[4]=0xF7`, payload is the response minus its first 5 bytes, page size `0x100`; then `00 00 07 E1 37` expecting `[4]=0x77` (**not fatal**). The accumulated payload is decrypted, then `0x8000` zero bytes are prepended.

**Write** (`write_mem` 624–702, `reflash_block` 712–923, `erase_mem` 925–966): the full image is encrypted first; blocks 3–10 of 11 are flashed (`block_modified = {0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0}`); erase is `00 00 07 E1 31 02 01 FF FF FF FF` then `delay(500)` then a 200 ms read; per block, `00 00 07 E1 34 04 33 <start24> <len24>` expecting `[4]=0x74`, then `pl_len/128` frames of `00 00 07 E1 B6 <blockaddr24>` plus 128 encrypted bytes expecting `[4]=0xF6` with `delay(200)` before each read, then `00 00 07 E1 37` expecting `[4]=0x77` (**not fatal**), then `00 00 07 E1 31 02 02 01` expecting `[4]=0x71 [5]=0x02 [6]=0x02` as the block's only success condition.

**Crypto** (`generate_seed_key` / `encrypt_payload` / `decrypt_payload`, legacy 970–1030): all three use an index transformation **byte-identical to `SsmProtocol::kIndexTransformationStock`** — verify this by direct comparison before reusing it, as wave 6a-1 did. Generation tables:
- seed key: `{0xF2CA, 0x2417, 0x21DE, 0x8475, 0x39AB, 0xF767, 0x6204, 0x6BE0, 0xBC63, 0x5988, 0x2845, 0x9846, 0xEB97, 0x99DE, 0xC7DB, 0xEFAE}`
- encrypt: `{0x3B61, 0x8BEF, 0x9E51, 0x1075}`
- decrypt: `{0x1075, 0x9E51, 0x8BEF, 0x3B61}` — the same four values reversed.

---

### Task 1: Types, hand-written plan builder, registration

**Files:** create `subaru_tcu_hitachi_m32r_can_{types.h,plan.h,plan.cpp,plan_test.cpp}`; modify `flash_types.h`, `src/backend/flash/ecu/BUILD.bazel`, `src/backend/flash/BUILD.bazel`, `bazel/portable_targets.bzl`.

**Interfaces produced:** `struct SubaruTcuHitachiM32rCanPlan { std::uint32_t request_id; std::uint32_t response_id; int bitrate; bool extended_id; std::uint32_t page_size; std::uint32_t write_frame_size; };` — `Result<FlashPlan> build_subaru_tcu_hitachi_m32r_can_plan(FlashOperation, std::string_view protocol_name, std::string_view mcu_type, std::optional<bytes::Bytes> image)` — `Status validate_subaru_tcu_hitachi_m32r_can_plan(const FlashPlan&)` — `FlashFamily::SubaruTcuHitachiM32rCan`.

Wire values: `request_id = 0x7E1`, `response_id = 0x7E9`, `bitrate = 500000`, `extended_id = false`, `page_size = 0x100`, `write_frame_size = 128`. Protocol name `sub_tcu_hitachi_m32r_can`, MCU `M32R_512KB`, `TransportKind::CanIso15765`.

Regions: read `{0x8000, 0x78000}` (divergence 2), write `{0x8000, 0x78000}`, erase one region `{0x8000, 0x78000}`, image size `0x80000`.

**Why hand-written rather than `single_window_plan`:** its builder and validator both reject `TestWrite` with `Unsupported`, which matches divergence 1 — but it also forces `erase_regions == {write_region}` and a single read/write window pair, and it cannot express the blocks-3-to-10 write set this family needs in Task 4. Follow `subaru_denso_sh7058_can_plan.cpp` (hand-written, on `master`) for the shape.

- [ ] **Step 1: Write the failing plan test.** Mirror `subaru_tcu_hitachi_m32r_kline_plan_test.cpp` (6a-1) for structure. Assert: `Read` builds with the exact wire values above and `read_region`; `Write` builds with the erase region and requires an image of exactly `0x80000` bytes; **`TestWrite` is rejected with `IsErr(ErrorKind::Unsupported)`** (divergence 1 — this is the safety assertion of the whole task); a foreign protocol name and a foreign MCU are each rejected; a `Write` with a wrong-sized image is rejected.
- [ ] **Step 2: Run it, confirm it fails** at analysis: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_can_plan_test` → no such target.
- [ ] **Step 3: Add the family to `flash_types.h`** — include, `FlashFamily::SubaruTcuHitachiM32rCan` appended under a `// Step 5 tail, wave 6a-2.` comment, the POD appended to `FamilyPlan`, a `FamilyTraits` specialization (`CanIso15765`), and `family_requires_kernel_v<...> = false` with a comment noting the resident on-board kernel. **Put the `family_requires_kernel_v` specialization in the existing grouped block after that template's primary declaration**, not beside `FamilyTraits` — the primary is declared later in the file and specializing it early is a hard compile error.
- [ ] **Step 4: Write `types.h` and the builder.** Include a header comment recording divergences 1 and 2 with their legacy line numbers.
- [ ] **Step 5: Add Bazel targets** — `_types`, `_plan` `cc_library`s and the `fastecu_portable_gtest` test, mirroring the 6a-1 block; add `_types` to the `flash_types` target's deps in `src/backend/flash/BUILD.bazel`.
- [ ] **Step 6: Register in `PORTABLE_PACKAGES`** — add `subaru_tcu_hitachi_m32r_can_plan` and `subaru_tcu_hitachi_m32r_can_types` by name to the `"src/backend/flash/ecu"` list, alphabetically. Verify with `bazel build --config=release //:portable_closure`.
- [ ] **Step 7: Run the tests** — the new plan test plus `bazel test --config=release //src/backend/flash/...` (proves the new `FamilyPlan` alternative did not break `flash_validation_test.cpp`'s `variant_size_v` static_assert, `flash_plan.cpp`'s switch, or `flash_printers.h`; all three are exhaustive and will need one new entry each).
- [ ] **Step 8: Commit.**

---

### Task 2: Executor transport setup and connect

**Files:** create `subaru_tcu_hitachi_m32r_can_executor.{h,cpp}`, `..._executor_test.cpp`; modify `src/backend/flash/ecu/BUILD.bazel`, `bazel/portable_targets.bzl`.

**Interfaces produced:** `class SubaruTcuHitachiM32rCanExecutor final : public ICanFlashExecutor` with `transport_setup` and `execute`.

`transport_setup` returns `iso15765_config_from(p)` — the plan POD satisfies the `Iso15765ConfigSource` concept, so use the helper in `flash_executor.h` rather than building the struct by hand.

- [ ] **Step 1: Write the failing tests** using `ScriptedCanFlashTransport` (mirror the CAN-side scripting in `subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp`). Cover: `transport_setup` returns `{0x7E1, 0x7E9, 500000, false}`; the **kernel-already-running short circuit** (frame 1 answers `71 02 02 03`, no further frames are sent); the full eight-step connect in order with byte-exact frames from the table above; each of the three fatal mismatches (steps 4, 5, 8) yields `BadResponse`; a seed response shorter than 10 bytes yields `BadResponse`; and — the one that pins divergence 3 — **the kernel-alive re-check at step 8 sends eight bytes, not six**.
- [ ] **Step 2: Run, confirm failure.**
- [ ] **Step 3: Write the executor**, mirroring 6a-1's decomposition (`framed`/`exchange`/`expect_prefix`/`request_prefix` helpers, cancellation checkpoints before write, after write, after read). Steps 2, 3 and 7 log and continue on mismatch — preserve that exactly; do not tighten them. Thread `IClock&` for the `delay(50)`/`delay(200)` pacing and honor cancellation in every sleep.
- [ ] **Step 4: Add the executor target, register it in `PORTABLE_PACKAGES`, run the tests, commit.**

---

### Task 3: The read path

**Files:** modify `subaru_tcu_hitachi_m32r_can_executor.{h,cpp}` and its test.

- [ ] **Step 1: Write the failing tests.** Cover: the `0x34` window setup carries `addr = 0x8000` and `len = 0x78000` (**this is divergence 2 — the assertion must fail if the underflowed `0xF00000` is used**); the dump loop issues `0x78000 / 0x100 = 1920` `0xB7` frames with incrementing addresses; a `0x37` stop frame is sent and a non-`0x77` answer is **not** fatal; the returned image is exactly `0x80000` bytes with the first `0x8000` all zero (divergence 4); and the decrypt table is applied. Give the scripted page payloads distinct, position-dependent values and assert reconstructed content at the first and last page — a size-only assertion would not catch an off-by-one in the 5-byte header strip.
- [ ] **Step 2: Run, confirm failure. Step 3: Implement. Step 4: Run, commit.**

---

### Task 4: The write path

**Files:** modify `subaru_tcu_hitachi_m32r_can_executor.{h,cpp}` and its test.

This is the task that can brick hardware. Nothing here is qualified.

- [ ] **Step 1: Write the failing tests.** Cover: the image is encrypted before any frame is sent; erase sends `00 00 07 E1 31 02 01 FF FF FF FF`; **an erase response shorter than 7 bytes is an error and no reflash frame follows** (divergence 5 — assert that `writesConsumed()` stops at the erase); blocks 3–10 are flashed and blocks 0–2 and 11+ are not; each block sends `0x34` setup then `len/128` `0xB6` frames of exactly 128 payload bytes each, then `0x37`, then the `0x31 02 02 01` checksum whose `71 02 02` answer is the block's only success; a non-`0x77` answer to `0x37` is not fatal; a missing or wrong checksum answer fails the block.
- [ ] **Step 2: Run, confirm failure. Step 3: Implement. Step 4: Run, commit.**

---

### Task 5: Cancellation and failure paths

**Files:** modify `subaru_tcu_hitachi_m32r_can_executor_test.cpp` only.

Mirror 6a-1's Task 4 **and its fix round**: a cancellation test must discriminate *which* checkpoint fired, not merely that `Cancelled` came back. Use a test-local tracing transport and tracing clock (6a-1's `TracingTransport`/`TracingClock` are the pattern) and assert the step sequence at explicit indices plus the total length.

- [ ] **Step 1:** Add tests for cancellation during the read loop, cancellation during the write loop, a plan built for another family reaching neither `transport_setup` nor the wire, and a transport error mid-dump. **Choose the cancellation trip count from this family's own connect length — do not copy 6a-1's number.**
- [ ] **Step 2: Run. Step 3: Commit.**

---

### Task 6: Desktop routing

**Files:** modify `src/platform/desktop/common/flash/flash_workflow.cpp`, `flash_workflow_test.cpp`, `BUILD.bazel`.

Mirror `SubaruTcuHitachiM32rKlineWorkflow` (6a-1, on `master`) but bind `DesktopCanFlashTransport`. Route `sub_tcu_hitachi_m32r_can` with `RouteMatch::Exact`.

- [ ] **Step 1: Write the failing test.** The Bazel target is `test_flash_workflow`. Prove read routes; prove **TestWrite yields a `FlashFailureStep` carrying `ErrorKind::Unsupported`** (divergence 1, at the app boundary); prove Write routes. The test file's default `request()` helper uses a different MCU — set `input.mcu = "M32R_512KB"` explicitly or the assertions pass for the wrong reason.
- [ ] **Step 2: Run, confirm failure. Step 3: Add the route, the `Route::Kind`, the factory case, the workflow class, the includes and the two deps. Step 4: Run, commit.**

---

### Task 7: Delete the legacy family and close the row

**Files:** delete the legacy operation and dialog; modify `mainwindow.{h,cpp}`, `src/ui/desktop/flash/tcu/BUILD.bazel`, `src/platform/desktop/common/flash/legacy/BUILD.bazel`, `scripts/check-legacy-flash-drain.py`, `docs/flash-qualification-matrix.md`.

- [ ] **Step 1: Delete both file pairs.** The dialog `#include` is in `mainwindow.h`, not `mainwindow.cpp`; the branch is in `mainwindow.cpp`. `src/ui/desktop/flash/tcu/BUILD.bazel` lists files explicitly — and after this deletion that package is empty, so remove the package and its reference from any parent. `legacy/BUILD.bazel`'s `legacy_flash_operations` target uses **explicit** `srcs`/`hdrs` (the glob-based target is the separate `family_operation_sources` filegroup) — remove the two entries. `legacy/tcu/` becomes empty; remove the directory.
- [ ] **Step 2: Remove exactly one `REMAINING` entry:** `tcu/flash_tcu_subaru_hitachi_m32r_can_operation.cpp`.
- [ ] **Step 3: Run `bazel test --config=release //:legacy_flash_drain`** — it fails in both directions, so a pass is meaningful.
- [ ] **Step 4: Update the matrix row** — `portable = yes`, `operations = read, write` (**not** `test_write`), `hardware_status = experimental`, naming the three suites. Notes must record all six divergences, with divergence 1 stated bluntly: *the legacy "test write" performed a real erase and write*. Add both `VERIFY:` items.
- [ ] **Step 5: Full verification** — `bazel test --config=release //...`, `prek run --all-files`, `bazel run //:clang_tidy_report_changed`, `bazel build --config=release //:portable_closure`.
- [ ] **Step 6: Commit. Do NOT push and do NOT open a PR** — compose the PR title and body into the task report for a human to use.

## Self-Review

**Spec coverage.** The wave-6 spec's 6a-2 row is this plan. Its per-family anatomy maps to Tasks 1–4, 6 and 7; its three test layers to Tasks 1 (plan), 2–5 (executor) and 6 (desktop). The spec's 6a rule is "no port additions, no ADR" — this plan adds no port surface, and the mandatory `PORTABLE_PACKAGES` registration is exactly what the spec was amended to require.

**Deviation from the spec.** The spec's 6a table lists this family as needing no changes beyond the template; it did not anticipate a hand-written plan builder. That is a builder choice inside the family, not a port or shared-helper change, so no amendment is needed — but Task 1 states the reasoning so a reviewer is not surprised.

**Placeholders.** None. Wire bytes, expected response codes, region bounds, block sets, frame sizes and crypto tables are all given exactly and were read from the legacy source. Code shapes point at named files on `master` rather than being re-transcribed, deliberately.

**Type consistency.** `SubaruTcuHitachiM32rCanPlan`'s six fields are used identically in Tasks 1–4. `build_/validate_subaru_tcu_hitachi_m32r_can_plan` keep their Task 1 signatures in Tasks 2 and 6. `FlashFamily::SubaruTcuHitachiM32rCan` and `Route::Kind::SubaruTcuHitachiM32rCan` are distinct types with matching names, per the established convention.
