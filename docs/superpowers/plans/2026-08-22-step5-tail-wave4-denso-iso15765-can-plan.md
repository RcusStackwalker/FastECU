# Step 5 Tail Wave 4 — Denso ISO-15765 Cluster Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate `FlashEcuSubaruDenso1N83M_1_5MCan`, `FlashEcuSubaruDensoSH72531Can`, `FlashEcuSubaruDensoSH72543CanDiesel`, and `FlashEcuSubaruDenso1N83M_4MCan` from legacy Qt flash operations to portable `FlashPlan` + `IFlashExecutor` pairs, taking `//:legacy_flash_drain` from 18 remaining families to 14.

**Architecture:** Each family gets a `<family>_types.h` plan struct, a `<family>_plan.{h,cpp}` builder/validator, and a `<family>_executor.{h,cpp}` synchronous cancellable executor under `src/backend/flash/ecu/`, driven through `UdsClient` over `CanFlashUdsChannel` on the already-portable `ICanFlashTransport`. Desktop integration is a `Route` entry plus a `SimpleCanFlashWorkflow` alias in `flash_workflow.cpp`; the legacy operation, its UI dialog, and its `mainwindow.cpp` dispatch branch are deleted in the same PR.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest/gmock, `bytes::Bytes`/`ByteView`, `fastecu::Result<T>`/`Error`, `SsmProtocol::calculateSeedKey`/`calculatePayload`.

**Spec:** [wave-4 design](../specs/2026-08-22-step5-tail-wave4-denso-iso15765-can-design.md) (commits `843b2c2`, `441efbb`)

## Global Constraints

- **Every exchange in a portable executor carries a comment citing the legacy file and line range it was transcribed from.** This is the only way a reviewer can check a port without hardware.
- **Byte-exact wire fidelity.** Preserve each family's own timeout constants, tolerance, loop bounds, and log strings. Do not normalize across families.
- **`test_write` is rejected before any I/O**, in both the plan builder and the executor's own re-validation.
- **Both programming branches (bench and in-car) are ported** for every family, each with its own scripted test.
- **Kernel-free:** `family_requires_kernel_v<...> = false` for all four plan types.
- Portable backend targets carry no Qt: no `QT_DEPS`, no Qt includes. Registration in `PORTABLE_ROOTS` (`scripts/check-portable-closure.py`) happens per PR, never in bulk.
- `//:legacy_flash_drain`'s `REMAINING` set may only shrink — one entry per migrated family.
- Every header needs `#pragma once`. Max line width 120 columns (`prek run --all-files` enforces).
- Hardware status stays `experimental`. Nothing reaches `proven` from unit tests.

**Gate to run before every commit:**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
prek run --all-files
```

---

## Shared protocol facts (all four families)

Learned by reading the legacy sources; these hold for every family unless a task says otherwise.

**Envelope.** Every request is `00 00 07 <id_low>` + PDU. Primary pair is request `0x7E0` / reply `0x7E8`, configured once via `configureIso15765Can(serial, "500000", 0x7E0, 0x7E8)`. The in-car branch additionally addresses `0x7A2`, `0x7DF`, `0x7E1`, `0x7B0` by varying the envelope's 4th byte over the same configured transport.

**`connect_bootloader` structure** (`1n83m_1_5m` lines 89-805, the reference):

| Phase | Request PDU | Expected reply | On mismatch | On empty |
|---|---|---|---|---|
| OBK-active probe | `10 5F` | `50 5F` | fall through | fall through |
| ECU ID | `AA` | `EA` … | log only | log only |
| VIN | `09 02` | `49 02` … | log only | log only |
| CAL ID | `09 04` | `49 04` … | log only | log only |
| CVN | `09 06` | `49 06` … | log only | log only |
| Access method | `10 5F` | `50 01` | log only | **return error** |
| Branch selector | `22 10 1D` | `62 10` | log only | **return error** |

Then branch on `received.at(7)`: `!= 0xFF` → in-car (lines 341–656), `== 0xFF` → bench (lines 657–803).

**Bench branch** (lines 659–802): `10 43` → `50 43` (fatal); `27 61` → `67 61` + 4 seed bytes at offsets 6–9 (fatal); `27 62` + 4 key bytes → `67 62` (fatal); `10 42` → then a `try_count < 50` re-read loop waiting for `50 42`.

**In-car branch** (lines 341–656): `10 5F` → `50 01` (log only; empty → return error), then a run of fire-and-forget writes whose replies are never inspected — `0x7A2:10 C0`, `0x7E0:10 63`, `0x7DF:10 03`, `0x7E1:10 63`, `0x7B0:10 03`, `0x7B0:85 02`, `0x7DF:85 02`, `0x7B0:85 02`, `0x7DF:85 02`, `0x7DF:28 03 01`, … (transcribe the full run from the cited lines) — then `27 61`/`27 62` seed+key on `0x7E0`, `10 5F` → `50 63`, `22 10 1D` → `62 10`, `10 62`, and a `try_count < 10` re-read loop waiting for `50 62`.

**Setup PDUs are standard UDS.** `34`/`35` + `04` (dataFormatIdentifier) + `44` (addressAndLengthFormatIdentifier: 4-byte address, 4-byte length) + 4-byte big-endian start + 4-byte big-endian length. For `1n83m_1_5m` that is `34 04 44 08 FA C0 00 00 17 3F 00`, expecting `74 20 01 05`. **This is why the region belongs in the plan:** `sh72543d` already computes these bytes from its region and the other three hardcode the identical result, so routing all four through the region field changes no bytes.

**Read loop:** `B7` + 4-byte big-endian address, reply `F7` + 256 encrypted bytes, decrypted per page. **Write loop:** `B6` + 4-byte big-endian address + 256 encrypted bytes, reply checked at `received.at(3) == 0xE8 && received.at(4) == 0xF6`. **Erase:** setup PDU, then `31 01 02 01 FF FF FF FF`, then a `try_count < 20` re-read loop for `71 01 02`. **Checksum verify:** `31 01 02 02 01`, absorbing `7F 31 78` pending.

**Crypto (identical in all four).** Seed table `{0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71, 0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B}`; encrypt `{0xC85B, 0x32C0, 0xE282, 0x92A0}`; decrypt `{0x92A0, 0xE282, 0x32C0, 0xC85B}`; `indextransformation` `{0x5,0x6,0x7,0x1,0x9,0xC,0xD,0x8,0xA,0xD,0x2,0xB,0xF,0x4,0x0,0x3,0xB,0x4,0x6,0x0,0xF,0x2,0xD,0x9,0x5,0xC,0x1,0xA,0x3,0xD,0xE,0x8}`.

## File structure

Per family `<f>` in {`subaru_denso_1n83m_1_5m_can`, `subaru_denso_sh72531_can`, `subaru_denso_sh72543_can_diesel`, `subaru_denso_1n83m_4m_can`}:

| File | Responsibility |
|---|---|
| `src/backend/flash/ecu/<f>_types.h` | The `<F>Plan` POD: CAN identity + region geometry |
| `src/backend/flash/ecu/<f>_plan.{h,cpp}` | `build_<f>_plan` / `validate_<f>_plan`; no I/O |
| `src/backend/flash/ecu/<f>_plan_test.cpp` | Pre-I/O rejection and geometry assertions |
| `src/backend/flash/ecu/<f>_executor.{h,cpp}` | `IFlashExecutor`: connect (both branches), read, erase, write, checksum |
| `src/backend/flash/ecu/<f>_executor_test.cpp` | Byte-exact scripted wire equivalence |

Modified per family: `src/backend/flash/flash_types.h`, `src/backend/flash/ecu/BUILD.bazel`, `src/platform/desktop/common/flash/flash_workflow.cpp`, `src/ui/desktop/mainwindow.cpp`, `src/ui/desktop/flash/ecu/BUILD.bazel`, `scripts/check-legacy-flash-drain.py`, `scripts/check-portable-closure.py`, `BUILD.bazel`, `docs/flash-qualification-matrix.md`.

Deleted per family: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_<f>_operation.{h,cpp}`, `src/ui/desktop/flash/ecu/flash_ecu_<f>.{h,cpp}`.

---

### Task 1: `FlashEcuSubaruDenso1N83M_1_5MCan` — the reference port

**Files:**
- Create: `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_types.h`
- Create: `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan_test.cpp`
- Create: `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp`
- Modify: `src/backend/flash/flash_types.h`, `src/backend/flash/ecu/BUILD.bazel`, `src/platform/desktop/common/flash/flash_workflow.cpp`, `src/ui/desktop/mainwindow.cpp:1248-1252`, `scripts/check-legacy-flash-drain.py`, `scripts/check-portable-closure.py`, `BUILD.bazel`, `docs/flash-qualification-matrix.md`
- Delete: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.{h,cpp}`, `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.{h,cpp}`

**Interfaces:**
- Consumes: `FlashPlanFields`/`validate_and_build` (`flash_plan.h`), `fatal_request`/`fatal_query`/`non_fatal_query`/`UdsExchangeContext` (`uds_client_exchange_common.h`), `CanFlashUdsChannel`, `PhaseSequence`/`PhaseReporter` (`flash_phase_progress.h`), `SsmProtocol::calculateSeedKey`/`calculatePayload`, `open_can_iso15765_transport`/`check_family_transport_match` (`flash_executor.h`), `ScriptedCanFlashTransport`, `FakeClock`, `RecordingEventSink`.
- Produces: `struct SubaruDenso1n83m_1_5mCanPlan`; `Result<FlashPlan> build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation, std::string_view protocol_name, std::string_view mcu_type, std::optional<bytes::Bytes> image)`; `Status validate_subaru_denso_1n83m_1_5m_can_plan(const FlashPlan&)`; `class SubaruDenso1n83m_1_5mCanExecutor final : public IFlashExecutor`. **The builder signature is fixed by `SimpleCanFlashWorkflow`'s `BuildPlan` template parameter — do not vary it.**

- [ ] **Step 1: Read the legacy source in full before writing any code**

Read `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp` end to end (1,536 lines), in particular:
- `execute()` lines 19–83
- `connect_bootloader()` lines 89-805, noting the branch at line 341 and that the in-car arm (341–656) and bench arm (657–803) both end in a kernel jump plus a bounded re-read loop with **different** subfunctions (`10 62`/`50 62`, `try_count < 10`) vs (`10 42`/`50 42`, `try_count < 50`)
- `read_memory()` lines 813-1074, noting lines 826–828 overwrite both arguments with `0x08FAC000` / `0x00173F00`, which equal `fblocks_N83M_1_5MB[1]` exactly
- `write_memory()` lines 1082-1160 and `reflash_block()` lines 1167-1359
- `erase_memory()` lines 1364-1466
- crypto lines 1474-1529

Confirm against `src/backend/definitions/kernelmemorymodels.h` that `fblocks_N83M_1_5MB[1] == {0x08FAC000, 0x00173F00}` and `numblocks == 3`.

- [ ] **Step 2: Write `subaru_denso_1n83m_1_5m_can_types.h`**

```cpp
#pragma once
#include <cstdint>

namespace fastecu::flash
{

// Legacy: flash_ecu_subaru_denso_1n83m_1_5m_can_operation.{h,cpp}. Single
// protocol variant. The region fields carry what legacy hardcoded in
// read_memory (lines 826-828) and the 0x34/0x35 setup PDUs (lines 842-852,
// 883-893): fblocks_N83M_1_5MB[1] exactly.
struct SubaruDenso1n83m_1_5mCanPlan
{
    std::uint32_t request_id;   // 0x7e0
    std::uint32_t response_id;  // 0x7e8
    int bitrate;                // 500000
    bool extended_id;           // false
    std::uint32_t lead_pad_len; // 0x10000, prepended to a read image as 0xFF
    std::uint32_t tail_pad_len; // 0x100, appended to a read image as 0xFF
};

} // namespace fastecu::flash
```

- [ ] **Step 3: Write the failing plan test**

Create `subaru_denso_1n83m_1_5m_can_plan_test.cpp` asserting, at minimum:

```cpp
TEST(SubaruDenso1n83m_1_5mCanPlan, ReadPlanCarriesMainFlashBlock)
{
    auto plan = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().start, 0x08FAC000u);
    EXPECT_EQ(plan->transfer_region().length, 0x00173F00u);
    EXPECT_THAT(plan->erase_regions(), IsEmpty());
    EXPECT_FALSE(plan->kernel().has_value());
}

TEST(SubaruDenso1n83m_1_5mCanPlan, TestWriteIsRejectedBeforeAnyIo)
{
    auto plan = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::TestWrite, kProtocol, kMcu, std::nullopt);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
}

TEST(SubaruDenso1n83m_1_5mCanPlan, WriteRequiresFullStartAlignedImage)
{
    // reflash_block indexes newdata[i + blockaddr - fblocks[0].start], so the
    // image must span fblocks[0].start..end, i.e. 0x184000 bytes.
    auto tooShort = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                           bytes::Bytes(0x173F00, 0x00));
    ASSERT_FALSE(tooShort.has_value());
    EXPECT_EQ(tooShort.error().kind, ErrorKind::InvalidConfig);

    auto ok = build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Write, kProtocol, kMcu,
                                                     bytes::Bytes(0x184000, 0x00));
    ASSERT_TRUE(ok.has_value()) << ok.error().detail;
    ASSERT_EQ(ok->erase_regions().size(), 1u);
    EXPECT_EQ(ok->erase_regions()[0].start, 0x08FAC000u);
    EXPECT_EQ(ok->erase_regions()[0].length, 0x00173F00u);
}

TEST(SubaruDenso1n83m_1_5mCanPlan, WrongProtocolAndWrongMcuAreRejected)
{
    EXPECT_FALSE(build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Read, "sub_ecu_denso_sh72531_can", kMcu,
                                                        std::nullopt).has_value());
    EXPECT_FALSE(build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation::Read, kProtocol, "SH72531",
                                                        std::nullopt).has_value());
}
```

with `constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_1_5m_can";` and `kMcu = "N83M_1_5MB";`.

- [ ] **Step 4: Run the plan test — verify it fails**

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_plan_test
```
Expected: FAIL — no such target.

- [ ] **Step 5: Write `subaru_denso_1n83m_1_5m_can_plan.{h,cpp}`**

Header:

```cpp
#pragma once
#include "src/backend/flash/flash_plan.h"

namespace fastecu::flash
{
Result<FlashPlan> build_subaru_denso_1n83m_1_5m_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                         std::string_view mcu_type, std::optional<bytes::Bytes> image);
Status validate_subaru_denso_1n83m_1_5m_can_plan(const FlashPlan& plan);
} // namespace fastecu::flash
```

Implementation follows `subaru_hitachi_m32r_can_plan.cpp` exactly in shape: a file-local `validate_identity(protocol, mcu)` checking the protocol string, `find_flash_device_index(mcu)`, the MCU name, and the `flashdevices[index]` geometry (`numblocks == 3`, `fblocks[1] == {0x08FAC000, 0x00173F00}`, `fblocks[0].start == 0x08F9C000`); then `validate_...` checking family/transport, the `SubaruDenso1n83m_1_5mCanPlan` wire parameters, the transfer region, kernel-freeness, `TestWrite` → `Unsupported`, read-plans-must-not-erase, and write image size `0x184000`; then `build_...` assembling `FlashPlanFields` and calling `validate_and_build` followed by `validate_...`.

Constants:

```cpp
constexpr std::string_view kProtocol = "sub_ecu_denso_1n83m_1_5m_can";
constexpr std::string_view kMcu = "N83M_1_5MB";
constexpr MemoryRegion kMainBlock{0x08FAC000, 0x00173F00};
constexpr std::uint32_t kImageStart = 0x08F9C000; // fblocks[0].start
constexpr std::size_t kImageSize = 0x184000;      // fblocks[0..2] summed
constexpr std::uint32_t kLeadPad = 0x10000;
constexpr std::uint32_t kTailPad = 0x100;
```

- [ ] **Step 6: Wire `flash_types.h`**

Add `#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_types.h"`; add `SubaruDenso1n83m_1_5mCan,` to `FlashFamily` under a `// Step 5 tail, wave 4.` comment; add `SubaruDenso1n83m_1_5mCanPlan` to the `FamilyPlan` variant; add:

```cpp
// Step 5 tail, wave 4. Jumps to the ECU's resident on-board kernel via
// 0x10 0x42 (bench) or 0x10 0x62 (in-car), uploading no image.
template <> inline constexpr bool family_requires_kernel_v<SubaruDenso1n83m_1_5mCanPlan> = false;
```

Add the matching `EXPECT_FALSE(family_requires_kernel_v<SubaruDenso1n83m_1_5mCanPlan>);` to `flash_types_test.cpp`.

- [ ] **Step 7: Add BUILD targets**

In `src/backend/flash/ecu/BUILD.bazel`, mirroring the `subaru_hitachi_m32r_can_*` targets: a `cc_library` `subaru_denso_1n83m_1_5m_can_types` (hdrs only), a `cc_library` `subaru_denso_1n83m_1_5m_can_plan` (deps `//src/backend/definitions:models`, `//src/backend/flash:flash_device_lookup`, `//src/backend/flash:flash_plan`, `//src/backend/flash:flash_validation`, `//src/backend/ports`), and a `fastecu_portable_gtest` `subaru_denso_1n83m_1_5m_can_plan_test`.

- [ ] **Step 8: Run the plan test — verify PASS**

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_plan_test
```

- [ ] **Step 9: Commit**

```bash
git add src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_{types.h,plan.h,plan.cpp,plan_test.cpp} \
        src/backend/flash/ecu/BUILD.bazel src/backend/flash/flash_types.h src/backend/flash/flash_types_test.cpp
git commit -m "feat: add portable Denso 1N83M 1.5M CAN flash plan"
```

- [ ] **Step 10: Write the failing executor test — bench connect + read success**

Model the file on `subaru_hitachi_m32r_can_executor_test.cpp`: `request()`/`response()` helpers that prepend the 4-byte id, an independently transcribed copy of the three crypto tables (**do not include the executor's tables — a wrong table must fail the test, not pass silently**), a `toWire()` helper, and scripted-sequence helpers. Add an id-parameterised request helper for the in-car branch:

```cpp
bytes::Bytes requestTo(std::uint32_t id, std::initializer_list<bytes::Byte> payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
```

Script the preliminary phase and the bench branch:

```cpp
void scriptPreliminaries(ScriptedCanFlashTransport& t, bytes::Byte branchByte)
{
    t.expectWrite(request({0x10, 0x5F}));            // OBK probe, miss
    t.queueRead(response({0x50, 0x01}));
    t.expectWrite(request({0xAA}));
    t.queueRead(response({0xEA, 0, 0, 0, 0, 1, 2, 3, 4, 5}));
    t.expectWrite(request({0x09, 0x02}));
    t.queueRead(response({0x49, 0x02, 'V', 'I', 'N'}));
    t.expectWrite(request({0x09, 0x04}));
    t.queueRead(response({0x49, 0x04, 'C', 'A', 'L'}));
    t.expectWrite(request({0x09, 0x06}));
    t.queueRead(response({0x49, 0x06, 0xAA, 0xBB}));
    t.expectWrite(request({0x10, 0x5F}));            // access method
    t.queueRead(response({0x50, 0x01}));
    // Branch selector. Byte 7 of the raw frame is payload index 3.
    t.expectWrite(request({0x22, 0x10, 0x1D}));
    t.queueRead(response({0x62, 0x10, 0x1D, branchByte}));
}

void scriptBenchConnect(ScriptedCanFlashTransport& t)
{
    scriptPreliminaries(t, 0xFF);
    t.expectWrite(request({0x10, 0x43}));
    t.queueRead(response({0x50, 0x43}));
    t.expectWrite(request({0x27, 0x61}));
    t.queueRead(response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    bytes::Bytes key{0x27, 0x62};
    const bytes::Bytes k = seedKey(kSeed);
    key.insert(key.end(), k.begin(), k.end());
    t.expectWrite(request(key));
    t.queueRead(response({0x67, 0x62}));
    t.expectWrite(request({0x10, 0x42}));
    t.queueRead(response({0x50, 0x42}));
}
```

First test case:

```cpp
TEST(SubaruDenso1n83m_1_5mCanExecutor, BenchReadReturnsPaddedImage)
{
    ScriptedCanFlashTransport transport;
    scriptBenchConnect(transport);
    scriptReadSetup(transport);                       // 0x34.../0x35... setup pair
    scriptFlashDump(transport, 0x08FAC000, 0x173F00, 0x100, 0xA5);
    scriptStopCommand(transport);

    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    SubaruDenso1n83m_1_5mCanExecutor executor;

    auto result = executor.execute(readPlan(), transport, clock, cancellation, events);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->read_bytes.has_value());
    const bytes::Bytes& rom = *result->read_bytes;
    EXPECT_EQ(rom.size(), 0x184000u);
    EXPECT_THAT(bytes::ByteView(rom).first(0x10000), Each(0xFF));           // leading pad
    EXPECT_EQ(rom[0x10000], 0xA5);                                          // decrypted payload
    EXPECT_THAT(bytes::ByteView(rom).last(0x100), Each(0xFF));              // tail pad
    EXPECT_TRUE(transport.scriptConsumed());
}
```

- [ ] **Step 11: Run — verify it fails to compile**

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test
```
Expected: FAIL — no such target / executor undeclared.

- [ ] **Step 12: Write `subaru_denso_1n83m_1_5m_can_executor.h`**

```cpp
#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruDenso1n83m_1_5mCanExecutor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
```

- [ ] **Step 13: Write `subaru_denso_1n83m_1_5m_can_executor.cpp`**

Follow `subaru_hitachi_m32r_can_executor.cpp` structurally: file-local `Ctx{cancellation, events, clock, uds, channel}`, `info`/`error` helpers, `kRejectionPrefix = "Wrong response from ECU: "`, thin `fatal_request`/`fatal_query`/`non_fatal_query` wrappers over `uds_client_exchange_common.h`, and the crypto table constants plus `seed_key`/`encrypt_rom`/`decrypt_page`.

Functions to write, each with its legacy line citation:
- `Status connect_bootloader(Ctx&, ICanFlashTransport& can)` — preliminary phase, then dispatch on the branch byte to `connect_in_car` or `connect_bench`.
- `Status connect_bench(Ctx&)` — lines 657–803.
- `Status connect_in_car(Ctx&, ICanFlashTransport& can)` — lines 341–656. The fire-and-forget writes go through per-id `CanFlashUdsChannel` instances constructed over the same `can` for the id being written; **their replies are read from `can` directly**, not through a channel, because legacy never validates the reply id. Do not add checks legacy lacks.
- `Result<bytes::Bytes> read_memory(Ctx&, const MemoryRegion&, PhaseReporter&)` — lines 813-1074, including the lead/tail `0xFF` padding that makes the returned image `0x184000` bytes.
- `Status erase_memory(Ctx&)` — lines 1364-1466, setup PDU then `31 01 02 01 FF FF FF FF` then the `try_count < 20` re-read loop.
- `Status reflash_block(Ctx&, bytes::ByteView image, PhaseReporter&)` — lines 1167-1359; encrypt the whole image once, `B6` sweep at 256-byte chunks indexed `image[addr - kImageStart]`, close-block retry, checksum verify.
- `Status write_memory(Ctx&, bytes::ByteView, PhaseSequence&)` — lines 1083–1161, `block_modified == {0,1,0}` so block 1 only.

`execute()` mirrors the wave-3 reference: `check_family_transport_match`, `validate_subaru_denso_1n83m_1_5m_can_plan`, early cancellation check, `open_can_iso15765_transport`, `PhaseSequence`, connect, then read or write. Repeat the `TestWrite` → `Unsupported` guard inside `execute()` so a hand-built plan cannot turn a dry run into a real write.

- [ ] **Step 14: Add the remaining executor test cases, one at a time, running the suite after each**

Required cases:
1. `BenchReadReturnsPaddedImage` (Step 10)
2. `InCarReadReturnsPaddedImage` — `scriptPreliminaries(t, 0x00)` then the full in-car write run with `requestTo(0x7A2, …)` / `requestTo(0x7DF, …)` / `requestTo(0x7E1, …)` / `requestTo(0x7B0, …)`, asserting the same image
3. `WriteErasesThenFlashesBlockOne` — full write path, asserting `B6` chunk bytes equal `toWire(image)` at the right offsets
4. `TestWriteIsRejectedBeforeAnyTransportCall` — hand-built plan; assert `Unsupported` **and** `transport.writesConsumed() == 0`
5. `ReadTimeoutPropagates` — `queue_error(ErrorKind::Timeout)`; assert `Timeout`
6. `ReadDisconnectPropagates` — `queue_error(ErrorKind::Disconnected)`; assert `Disconnected`
7. `NegativeResponseDuringConnectFails` — `7F 27 35` at the seed step; assert `BadResponse`
8. `CancellationMidReadReturnsCancelled` — cancel after N pages; assert `Cancelled` and that no image is returned
9. `EmptyBranchSelectorReplyFails` — `queue_no_frame()` at `22 10 1D`; assert an error (legacy returns `STATUS_ERROR` there)
10. `EraseRetryExhaustionFails` — 20 non-matching reads; assert `BadResponse`

- [ ] **Step 15: Add executor BUILD targets and run the full suite**

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test
```
Expected: all PASS.

- [ ] **Step 16: Wire the desktop workflow**

In `src/platform/desktop/common/flash/flash_workflow.cpp`: add both includes; add

```cpp
using SubaruDenso1n83m_1_5mCanWorkflow =
    SimpleCanFlashWorkflow<SubaruDenso1n83m_1_5mCanExecutor, &build_subaru_denso_1n83m_1_5m_can_plan>;
```

add `SubaruDenso1n83m_1_5mCan` to `Route::Kind`, `{"sub_ecu_denso_1n83m_1_5m_can", SubaruDenso1n83m_1_5mCan},` to `kRoutes`, and the matching `case` to `tryCreate`. Add a routing assertion to `flash_workflow_test.cpp`.

- [ ] **Step 17: Delete the legacy family and its dialog**

Delete the two legacy operation files and `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.{h,cpp}`; remove the dialog from `src/ui/desktop/flash/ecu/BUILD.bazel` (`srcs`, `MOC_HDRS`); delete the `mainwindow.cpp:1248-1252` dispatch branch and its include.

- [ ] **Step 18: Shrink the ratchet and register the portable closure**

Remove `"ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp"` from `REMAINING` in `scripts/check-legacy-flash-drain.py`. Add `subaru_denso_1n83m_1_5m_can_types`, `_plan`, `_executor` to `PORTABLE_ROOTS` in `scripts/check-portable-closure.py` and to the two `genquery` lists in `BUILD.bazel`. Verify non-vacuously: temporarily remove one target and confirm `//:portable_closure` fails.

- [ ] **Step 19: Flip the matrix row**

In `docs/flash-qualification-matrix.md`, set `portable=yes`, `hardware_status=experimental`, `automated_evidence` to `` `subaru_denso_1n83m_1_5m_can_plan_test`, `subaru_denso_1n83m_1_5m_can_executor_test` @ Wave 4 ``, and write the notes: `test_write` rejected before I/O and why it matters, both branches ported, and the `romsize`-vs-`fblocks` inconsistency.

- [ ] **Step 20: Run the full gate and commit**

Run the Global Constraints gate. Then:

```bash
git add -A
git commit -m "feat: migrate Denso 1N83M 1.5M CAN flash workflow"
```

---

### Task 2: Code review before continuing the wave

- [ ] **Step 1:** Invoke `superpowers:requesting-code-review` on Task 1's diff. Tasks 3–5 replicate its shape four times over, so a structural problem must be caught here, not after 6,000 lines.
- [ ] **Step 2:** Apply the feedback to Task 1 before starting Task 3.

---

### Task 3: `FlashEcuSubaruDensoSH72531Can`

**Files:** the same five-file-plus-wiring shape as Task 1, named `subaru_denso_sh72531_can_*`. Deletes `ecu/flash_ecu_subaru_denso_sh72531_can_operation.{h,cpp}`, `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh72531_can.{h,cpp}`, and `mainwindow.cpp:1238-1242`.

**Interfaces:** Produces `SubaruDensoSh72531CanPlan`, `build_subaru_denso_sh72531_can_plan`, `validate_subaru_denso_sh72531_can_plan`, `SubaruDensoSh72531CanExecutor` — same signatures as Task 1.

This family is the closest sibling to Task 1: strict tolerance, hardcoded addressing, identical branch structure. Only the values below differ.

- [ ] **Step 1: Read `flash_ecu_subaru_denso_sh72531_can_operation.cpp` in full (1,534 lines).** Confirm `fblocks_SH72531[1] == {0x8000, 0x137F00}` and `numblocks == 3` in `kernelmemorymodels.h`.

- [ ] **Step 2: Write the types header**

```cpp
#pragma once
#include <cstdint>

namespace fastecu::flash
{

// Legacy: flash_ecu_subaru_denso_sh72531_can_operation.{h,cpp}. Region fields
// carry what legacy hardcoded in read_memory (lines 828-830) and the 0x34/0x35
// setup PDUs: fblocks_SH72531[1] exactly.
struct SubaruDensoSh72531CanPlan
{
    std::uint32_t request_id;   // 0x7e0
    std::uint32_t response_id;  // 0x7e8
    int bitrate;                // 500000
    bool extended_id;           // false
    std::uint32_t lead_pad_len; // 0x8000, prepended to a read image as 0xFF
    std::uint32_t tail_pad_len; // 0x100, appended to a read image as 0xFF
};

} // namespace fastecu::flash
```

- [ ] **Step 3: Write the failing plan test** with these exact values: `kProtocol = "sub_ecu_denso_sh72531_can"`, `kMcu = "SH72531"`, transfer region `{0x8000, 0x137F00}`, image base `0x0`, image size `0x140000`, erase region `{0x8000, 0x137F00}`. Four cases, named as in Task 1 Step 3: `ReadPlanCarriesMainFlashBlock` (asserts the region above, empty erase regions, no kernel), `TestWriteIsRejectedBeforeAnyIo` (asserts `ErrorKind::Unsupported`), `WriteRequiresFullStartAlignedImage` (a `0x137F00` image is refused with `InvalidConfig`; a `0x140000` image is accepted and yields one erase region equal to the transfer region), and `WrongProtocolAndWrongMcuAreRejected` (asserts both `sub_ecu_denso_1n83m_1_5m_can` and MCU `N83M_1_5MB` are refused).

- [ ] **Step 4: Run — verify it fails to compile.**

- [ ] **Step 5: Write the plan `.h`/`.cpp`** with those constants.

- [ ] **Step 6: Wire `flash_types.h`** — `SubaruDensoSh72531Can` enum value, variant alternative, `family_requires_kernel_v<SubaruDensoSh72531CanPlan> = false`, `flash_types_test.cpp` assertion.

- [ ] **Step 7: Add BUILD targets, run the plan test, verify PASS, commit.**

- [ ] **Step 8: Write the failing executor test.** Same ten cases as Task 1 Step 14, with the setup PDU `34 04 44 00 00 80 00 00 13 7F 00` (start `0x00008000`, length `0x00137F00`), read sweep over `{0x8000, 0x137F00}`, and an expected image of `0x140000` bytes: `0x8000` of `0xFF`, the decrypted payload, `0x100` of `0xFF`.

- [ ] **Step 9: Run — verify it fails to compile.**

- [ ] **Step 10: Write the executor `.h`/`.cpp`**, transcribing from this family's own line numbers. Its `read_memory` overwrite is at lines 829–831; both branches, the erase loop, and the checksum verify have the same structure as Task 1.

- [ ] **Step 11: Add the remaining executor tests one at a time; run the suite after each; add BUILD targets; verify full PASS.**

- [ ] **Step 12: Wire the desktop workflow** — `SubaruDensoSh72531Can` route with prefix `"sub_ecu_denso_sh72531_can"`, workflow alias, `tryCreate` case, routing assertion.

- [ ] **Step 13: Delete the legacy family and dialog; shrink `REMAINING`; register `PORTABLE_ROOTS` and the `BUILD.bazel` genqueries.**

- [ ] **Step 14: Flip the matrix row** — same note set as Task 1 minus the `romsize` item (SH72531's table is self-consistent).

- [ ] **Step 15: Run the full gate; commit.**

---

### Task 4: `FlashEcuSubaruDensoSH72543CanDiesel`

**Files:** same shape, named `subaru_denso_sh72543_can_diesel_*`. Deletes `ecu/flash_ecu_subaru_denso_sh72543_can_diesel_operation.{h,cpp}`, `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh72543_can_diesel.{h,cpp}`, and `mainwindow.cpp:1213-…`.

**Interfaces:** Produces `SubaruDensoSh72543CanDieselPlan`, `build_subaru_denso_sh72543_can_diesel_plan`, `validate_subaru_denso_sh72543_can_diesel_plan`, `SubaruDensoSh72543CanDieselExecutor`.

This family diverges from Tasks 1 and 3 in four specific ways. Everything else is the same shape.

- [ ] **Step 1: Read `flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp` in full (1,556 lines).** Note the four divergences:
  1. **ECU ID uses `22 F1 82` → `62 F1 82`**, not `AA` → `EA`; the reply trim is `remove(0, 7)` keeping everything, not `remove(0, 8)` truncated to 5.
  2. **The `0x34`/`0x35` setup PDUs are computed** from `start_addr`/`length` rather than hardcoded, and its `read_memory` overwrite at lines 812-813 is **commented out**, so `execute()`'s arguments (`fblocks[0].start`, `fblocks[0].len`) reach it.
  3. **`erase_memory(const flashdev_t *fdt, unsigned blockno)`** takes the block, called as `erase_memory(&flashdevices[mcu_type_index], 0)`.
  4. **`block_modified[16] = {1}`** — block 0, not block 1. `fblocks_SH72543d` has `numblocks == 1` with `fblocks[0] == {0x8000, 0x1F7F00}`; its `0x0` entry is commented out in `kernelmemorymodels.h`.
  5. **`reflash_block` indexes `newdata[i + blockctr * blocksize]`** — an image base of `0x8000`, whereas `read_memory` prepends `0x8000` of `0xFF` and hands back an image based at `0x0`. **This port indexes from address `0` instead**, per the spec's write-base divergence. See Step 8a.
  Also note it carries **one** commented-out `return STATUS_ERROR` (line 672) and uses `serial_read_timeout` (2000 ms) where the siblings use `serial_read_short_timeout` (200 ms). Preserve both.

- [ ] **Step 2: Add `kSidReadDataByIdentifier`**

`0x22` is not yet in `src/algorithms/protocol/uds/uds_service_ids.h`. Add beside the other ISO 14229-1 entries:

```cpp
constexpr bytes::Byte kSidReadDataByIdentifier = 0x22; // ISO 14229-1
```

Tasks 1 and 3 use `0x22` too (the branch selector `22 10 1D`); if they already added it, skip this step.

- [ ] **Step 3: Write the types header** — same fields, `lead_pad_len = 0x8000`, `tail_pad_len = 0x100`.

- [ ] **Step 4: Write the failing plan test** with `kProtocol = "sub_ecu_denso_sh72543_can_diesel"`, `kMcu = "SH72543d"`, transfer region `{0x8000, 0x1F7F00}`, **image base `0x0`** (the divergence), image size `0x200000`, erase region `{0x8000, 0x1F7F00}`. Add a case asserting the plan accepts `numblocks == 1` geometry, since this is the only family in the wave whose flash table has one block, and a case pinning the image base explicitly:

```cpp
TEST(SubaruDensoSh72543CanDieselPlan, WriteImageIsBasedAtAddressZero)
{
    // Legacy reflash_block indexed newdata[i + blockctr * blocksize], an image
    // base of 0x8000, while read_memory returned an image based at 0x0 -- so a
    // full ROM was written 0x8000 low. This port bases the write image at 0x0,
    // matching the read output and the three sibling families.
    auto plan = build_subaru_denso_sh72543_can_diesel_plan(FlashOperation::Write, kProtocol, kMcu,
                                                           bytes::Bytes(0x200000, 0x00));
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->image()->size(), 0x200000u);
    EXPECT_EQ(plan->transfer_region().start, 0x8000u);
}
```

- [ ] **Step 5: Run — verify it fails to compile.**

- [ ] **Step 6: Write the plan `.h`/`.cpp`; wire `flash_types.h`; add BUILD targets; run the plan test; verify PASS; commit.**

- [ ] **Step 7: Write the failing executor test.** Same ten cases, with the ECU-ID exchange scripted as `t.expectWrite(request({0x22, 0xF1, 0x82})); t.queueRead(response({0x62, 0xF1, 0x82, 'I', 'D'}));`, setup PDUs `34 04 44 00 00 80 00 00 1F 7F 00` / `35 04 44 00 00 80 00 00 1F 7F 00`, and an expected image of `0x200000` bytes.

- [ ] **Step 8: Write the executor `.h`/`.cpp`.** Build the setup PDU with `composeBe(uds::kSidRequestDownload, bytes::Byte(0x04), bytes::Byte(0x44), region.start, region.length)` rather than a literal, matching this family's computed form — and note in a comment that this produces bytes identical to Tasks 1 and 3's literals.

- [ ] **Step 8a: Apply the write-base divergence and test it**

Index the write image as `encrypted[addr]` where `addr` is the absolute flash address (`0x8000 + blockctr * 256`), **not** `encrypted[blockctr * 256]`. Carry this comment at the indexing site:

```cpp
// DELIBERATE DIVERGENCE (see the wave-4 design's write-base section).
// Legacy indexed newdata[i + blockctr * blocksize]
// (flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp:1217), an image
// base of 0x8000, while its own read_memory returns an image based at 0x0.
// A full 0x200000 ROM was therefore written 0x8000 low across every block.
// This port bases the image at 0x0, matching the read output and the three
// sibling families. Unverified on hardware -- first item on this family's
// bench checklist.
```

Add the executor test that pins it:

```cpp
TEST(SubaruDensoSh72543CanDieselExecutor, WriteTakesBytesFromTheAbsoluteAddress)
{
    // A 0x200000 image whose byte at 0x8000 is 0xC3 must put 0xC3 in the first
    // written chunk -- not the byte at offset 0.
    bytes::Bytes rom(0x200000, 0x00);
    rom[0x8000] = 0xC3;
    // ... script connect + erase + the first 0xB6 chunk, asserting its payload
    // equals toWire(rom.subspan(0x8000, 0x1F7F00)).first(256) ...
}
```

- [ ] **Step 9: Add the remaining executor tests one at a time; run after each; add BUILD targets; verify full PASS.**

- [ ] **Step 10: Wire the desktop workflow** — prefix `"sub_ecu_denso_sh72543_can_diesel"`.

- [ ] **Step 11: Delete the legacy family and dialog; shrink `REMAINING`; register the closure roots.**

- [ ] **Step 12: Flip the matrix row**, adding two notes: the write-base divergence, flagged as the first item a bench operator must verify; and that cfg declares `<kernel>ssmk_can_tp_sh72543d_euro6.bin</kernel>` and `<kernel_addr>0xFFF80000</kernel_addr>` which this family does not use.

- [ ] **Step 13: Run the full gate; commit.**

---

### Task 5: `FlashEcuSubaruDenso1N83M_4MCan` — the tolerant outlier

**Files:** same shape, named `subaru_denso_1n83m_4m_can_*`. Deletes `ecu/flash_ecu_subaru_denso_1n83m_4m_can_operation.{h,cpp}`, `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_1n83m_4m_can.{h,cpp}`, and `mainwindow.cpp:1243-1247`.

**Interfaces:** Produces `SubaruDenso1n83m_4mCanPlan`, `build_subaru_denso_1n83m_4m_can_plan`, `validate_subaru_denso_1n83m_4m_can_plan`, `SubaruDenso1n83m_4mCanExecutor`.

- [ ] **Step 1: Read `flash_ecu_subaru_denso_1n83m_4m_can_operation.cpp` in full (1,552 lines).** This family's defining property is **seven commented-out `return STATUS_ERROR` statements** — at lines 305, 335, 369 (in `connect_bootloader`) and 876, 883, 917, 924 (in `read_memory`'s two setup checks). At each of those seven points the family **logs and proceeds** where Tasks 1 and 3 abort. It also issues a **duplicate `read_serial_data` after the jump-to-kernel** (an extra read its siblings do not perform) and uses bare `200`/`500` literals where the siblings use named constants. Preserve all of it.

- [ ] **Step 2: Write the types header** — `lead_pad_len = 0x10000`, `tail_pad_len = 0x100`.

- [ ] **Step 3: Write the failing plan test** with `kProtocol = "sub_ecu_denso_1n83m_4m_can"`, `kMcu = "N83M_4MB"`, transfer region `{0x08FAC000, 0x3D3F00}`, image start `0x08F9C000`, image size `0x3E4000`, erase region `{0x08FAC000, 0x3D3F00}`.

- [ ] **Step 4: Run — verify it fails to compile.**

- [ ] **Step 5: Write the plan `.h`/`.cpp`; wire `flash_types.h`; add BUILD targets; run the plan test; verify PASS; commit.**

- [ ] **Step 6: Write the failing executor test, tolerance cases first.**

The tolerance is this family's whole point, so assert it **positively** — a test that only checks the happy path would pass against a wrongly-strict port:

```cpp
TEST(SubaruDenso1n83m_4mCanExecutor, ProceedsPastMalformedConnectResponses)
{
    ScriptedCanFlashTransport transport;
    // The three connect checks whose `return STATUS_ERROR` is commented out
    // (legacy lines 306, 336, 370): a negative response must NOT stop the
    // sequence -- the executor is expected to log and continue.
    scriptPreliminariesWithNegativeIdReplies(transport);
    scriptBenchConnectTail(transport);
    scriptReadSetupWithNegativeReplies(transport);   // legacy lines 876/883/917/924
    scriptFlashDump(transport, 0x08FAC000, 0x3D3F00, 0x100, 0x5A);
    scriptStopCommand(transport);

    // ... execute ...
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->read_bytes->size(), 0x3E4000u);
    EXPECT_TRUE(transport.scriptConsumed());
}
```

Add the mirror-image assertion to Tasks 1 and 3's suites if not already present: a negative response at the *same* exchanges must return an error there. Those two tests together are what prevent the tolerance from being silently normalized in either direction.

- [ ] **Step 7: Run — verify it fails to compile.**

- [ ] **Step 8: Write the executor `.h`/`.cpp`.** Each of the seven tolerated checks gets an inline comment naming its legacy line and stating that the early return is intentionally absent. Reproduce the duplicate post-jump read explicitly, with its own comment.

- [ ] **Step 9: Add the remaining executor tests (the other nine cases from Task 1 Step 14) one at a time; run after each; add BUILD targets; verify full PASS.**

- [ ] **Step 10: Wire the desktop workflow** — prefix `"sub_ecu_denso_1n83m_4m_can"`. Note `kRoutes` is matched by `starts_with`, and `"sub_ecu_denso_1n83m_1_5m_can"` and `"sub_ecu_denso_1n83m_4m_can"` are not prefixes of one another, so ordering between them does not matter.

- [ ] **Step 11: Delete the legacy family and dialog; shrink `REMAINING` — this empties the wave's four entries; register the closure roots.**

- [ ] **Step 12: Flip the matrix row**, noting the seven tolerated checks and the duplicate post-jump read.

- [ ] **Step 13: Run the full gate; commit.**

---

### Task 6: Cluster factoring decision

**Files:**
- Possibly create: `src/backend/flash/ecu/denso_iso15765_can_common.{h,cpp}` and `_test.cpp`
- Modify: the four executors, `src/backend/flash/ecu/BUILD.bazel`

Per the spec, the expected outcome is small: the only four-way-identical artifact is the crypto key-table set. **A factoring PR that lands nothing but this task's documentation is a valid outcome** and must not be treated as a failed wave.

- [ ] **Step 1: Diff the four finished executors** and list every block that is now byte-identical across all four.

- [ ] **Step 2: Apply the guardrail.** Factor only across a *data* difference — region, pad size, block mask, key table. Do **not** factor across a control-flow or tolerance difference; Task 5's seven tolerated checks and Task 4's erase signature must stay in their own executors.

- [ ] **Step 3: If the key tables are the only common artifact,** create `denso_iso15765_can_common.h` holding the three `constexpr` tables plus the shared `indextransformation`, and have all four executors include it. Add `denso_iso15765_can_common_test.cpp` asserting the tables produce known seed-key and payload vectors.

- [ ] **Step 4: Run all four executor suites** — they are the guard that the factoring changed no behavior.

```bash
bazel test --config=release //src/backend/flash/ecu:all
```

- [ ] **Step 5: Record the decision** in the spec's cluster-factoring section — what was factored, or that nothing was, and why.

- [ ] **Step 6: Run the full gate; commit.**

---

### Task 7: Wave closeout

**Files:** `docs/modularization-plan.md`, `docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md`

- [ ] **Step 1: Add the wave-4 row to `docs/modularization-plan.md`**, after the wave-3 entry, in the established form: the four family names, "merged", and "Takes the drain from 18 remaining families to 14."

- [ ] **Step 2: Record the two amendments in the tail design** — that wave 4's substrate payoff proved small (zero of six protocol functions four-way identical), and that both programming branches were ported rather than only the bench path.

- [ ] **Step 3: Verify the ratchet arithmetic.**

```bash
python3 scripts/check-legacy-flash-drain.py
```
Expected: PASS with 14 entries remaining.

- [ ] **Step 4: Run the full gate; commit.**

---

## Notes for the executor of this plan

- **The legacy `.cpp` is the only source of truth for correct bytes, and it is deleted in the same PR as the port.** Transcribe from it, cite it, and do not infer a byte from a sibling family.
- **Where this plan gives a line range rather than the literal bytes, read the range.** The ranges were verified against master `20892df`, this branch's base; if the file has moved under you, re-locate by function name rather than trusting the number.
- **`try_count` bounds differ per branch and per family** (50 for bench jump, 10 for in-car jump, 20 for erase, 6 for close-block). Copy them; do not unify them.
- **`RecordingEventSink` captures log lines.** Where a legacy log string has a portable counterpart, assert it character-for-character — that is how the wave keeps operator-visible behavior stable.
