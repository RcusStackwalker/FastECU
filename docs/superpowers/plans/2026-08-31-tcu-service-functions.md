# TCU Service Functions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `FlashTcuSubaruDensoSH705xCanOperation`'s `tcuAction` 2, 3 and 4 — TCU relearn, read parameters, set parameters — out of the legacy Qt flash operation into a new portable `src/backend/service_functions` package driven over the existing `ISsmTransport` port, correcting the six defects that stop all three from working today.

**Architecture:** A portable `ServiceFunctionSession` step machine: `transport_setup()` is pure and returns the transport configuration the session needs; `resume()` runs bounded, cancellable I/O until it hits an operator gate, completion, or failure; `submit()` answers a gate. Backend owns no threads and shows no dialogs. A desktop worker in `src/platform/desktop/common/service_functions` applies the configuration to `SerialPortActions`, runs `resume()` off the GUI thread, and marshals gates to a new `ServiceFunctionDialog`.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest 1.17.0.bcr.2 via `fastecu_portable_gtest`, Qt 6.8.3 for the desktop half only.

**Spec:** [TCU service functions design](../specs/2026-08-31-tcu-service-functions-design.md)

## Global Constraints

- **Portable means portable.** Everything under `src/backend/service_functions` is Qt-free, thread-free and filesystem-free. Adding a Qt dep there fails `//:portable_closure`, not the compiler.
- **Namespace** is `fastecu::service_functions`. Every header carries `#pragma once` (enforced by prek).
- **Bytes** are `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView` from `src/algorithms/protocol/bytes.h`. `QByteArray` appears only in the desktop tasks, converted through `qt_bytes.h`.
- **Errors** are `fastecu::Result<T>` / `fastecu::Status`, checked with `.has_value()`, never the implicit `operator bool`. Exceptions never cross a port.
- **Every exchange carries a legacy line citation** in a comment, in the form `// legacy flash_tcu_subaru_denso_sh705x_can_operation.cpp:NNN`. This is the only way a reviewer can check a port with no bench. All line numbers below refer to `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_denso_sh705x_can_operation.cpp`.
- **Timeouts, verbatim from the legacy class header** (`flash_tcu_subaru_denso_sh705x_can_operation.h:59-64`): `receive_timeout = 500`, `serial_read_short_timeout = 200`.
- **Protocol names accepted:** `sub_tcu_denso_sh7055_can` and `sub_tcu_denso_sh7058_can`. Anything else is `ErrorKind::Unsupported`.
- **These sessions produce six of the seven `ErrorKind` values.** `InvalidConfig` has no producer: protocol rejection is `Unsupported`, and the parameter bounds are enforced by `std::uint8_t` / `std::uint16_t` rather than a runtime check. Do not invent a check that cannot fail to "complete" the taxonomy.
- **`//:legacy_flash_drain` must not change in any task.** The legacy class keeps `tcuAction` 1 and its `serial_port_actions.h` include; its ratchet entry is wave 5's to remove.
- **Gate per task:**
  ```sh
  bazel build -k --config=release //:fastecu //tests/...
  bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
              //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
  ```
  plus `prek run --all-files` before every commit. Tasks 1-5 are PR 1; tasks 6-9 are PR 2.

---

### Task 1: Package skeleton, types, and portable-closure registration

**Files:**
- Create: `src/backend/service_functions/service_function_types.h`
- Create: `src/backend/service_functions/service_function_session.h`
- Create: `src/backend/service_functions/service_function_types_test.cpp`
- Create: `src/backend/service_functions/BUILD.bazel`
- Create: `src/backend/service_functions/README.md`
- Modify: `BUILD.bazel` (the `portable_backend_closure` genquery at line 33 and the `portable_closure` `py_test` `data` at line 176)
- Modify: `scripts/check-portable-closure.py` (`PORTABLE_ROOTS`, line 34)

**Interfaces:**
- Consumes: nothing.
- Produces: `fastecu::service_functions::{SsmTransportConfig, OperatorGateId, GateResponse, TcuParameterReadout, RelearnOutcome, SetParametersOutcome, ServiceFunctionOutcome, GateStep, CompletedStep, FailedStep, ServiceFunctionStep, ServiceFunctionSession}` and the Bazel targets `//src/backend/service_functions:service_function_types` and `:service_function_session`.

- [ ] **Step 1: Write the failing test**

Create `src/backend/service_functions/service_function_types_test.cpp`:

```cpp
#include "src/backend/service_functions/service_function_types.h"

#include <gtest/gtest.h>

namespace fastecu::service_functions
{
namespace
{

TEST(SsmTransportConfig, DefaultsToTheTcuIso15765Pair)
{
    const SsmTransportConfig config;
    EXPECT_EQ(config.framing, SsmTransportConfig::Framing::Iso15765);
    EXPECT_EQ(config.bitrate_or_baud, 500000);
    EXPECT_EQ(config.request_id, 0x7e1U);
    EXPECT_EQ(config.response_id, 0x7e9U);
    EXPECT_FALSE(config.add_iso14230_header);
}

TEST(SsmTransportConfig, TesterAndTargetAreZeroUnlessKline)
{
    // The two ISO-15765 sessions never reference them: legacy sets them only
    // on the K-Line path (legacy :151-152) and uses them only there (:215).
    const SsmTransportConfig config;
    EXPECT_EQ(config.tester_id, 0x00);
    EXPECT_EQ(config.target_id, 0x00);
}

TEST(ServiceFunctionStep, HoldsGateCompletedAndFailedAlternatives)
{
    const ServiceFunctionStep gate = GateStep{OperatorGateId::RelearnEngineRunning};
    ASSERT_TRUE(std::holds_alternative<GateStep>(gate));
    EXPECT_EQ(std::get<GateStep>(gate).id, OperatorGateId::RelearnEngineRunning);

    const ServiceFunctionStep done = CompletedStep{TcuParameterReadout{}};
    ASSERT_TRUE(std::holds_alternative<CompletedStep>(done));

    const ServiceFunctionStep bad = FailedStep{Error{ErrorKind::BadResponse, "nope"}};
    ASSERT_TRUE(std::holds_alternative<FailedStep>(bad));
    EXPECT_EQ(std::get<FailedStep>(bad).error.kind, ErrorKind::BadResponse);
}

TEST(TcuParameterReadout, ValueTypesEncodeTheLegacyPromptBounds)
{
    // legacy :162-202 -- eight 0-255 prompts and one 0-65535 prompt. The value
    // model enforces those bounds instead of a runtime range check.
    static_assert(sizeof(TcuParameterReadout::input_clutch) == 1);
    static_assert(sizeof(TcuParameterReadout::awd_clutch_torque) == 2);
    SUCCEED();
}

} // namespace
} // namespace fastecu::service_functions
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/service_functions:service_function_types_test`
Expected: FAIL — the package does not exist yet (`no such package 'src/backend/service_functions'`).

- [ ] **Step 3: Write the types header**

Create `src/backend/service_functions/service_function_types.h`:

```cpp
#pragma once

#include <cstdint>
#include <variant>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/error.h"

namespace fastecu::service_functions
{

// What the platform must apply to its serial facade before handing this
// session an ISsmTransport. ISsmTransport is a bare byte pipe with no
// configure(), so the configuration travels as data -- the same idea as
// IFlashExecutor::transport_setup returning KlineConfig / Iso15765Config.
struct SsmTransportConfig
{
    enum class Framing
    {
        Iso15765,
        Kline14230,
    };

    Framing framing{Framing::Iso15765};
    int bitrate_or_baud{500000};
    std::uint32_t request_id{0x7e1};  // ISO-15765 only
    std::uint32_t response_id{0x7e9}; // ISO-15765 only
    bytes::Byte tester_id{0x00};      // K-Line only; legacy :151
    bytes::Byte target_id{0x00};      // K-Line only; legacy :152
    bool add_iso14230_header{false};  // sessions self-frame via addHeader

    bool operator==(const SsmTransportConfig&) const = default;
};

// Semantic identity only: the desktop owns every word the operator reads,
// exactly as ConfirmationSpec::Id does for flash.
enum class OperatorGateId
{
    RelearnStaticSetup,   // legacy :648
    RelearnEngineRunning, // legacy :735
};

enum class GateResponse
{
    Accept,
    Decline,
};

// legacy :611-624 decodes these nine values from response bytes 5..14.
struct TcuParameterReadout
{
    bytes::Byte input_clutch{};             // 0x16c, byte 5
    bytes::Byte high_low_reverse_clutch{};  // 0x16d, byte 6
    bytes::Byte direct_clutch{};            // 0x16e, byte 7
    bytes::Byte front_brake{};              // 0x16f, byte 8
    std::uint16_t awd_clutch_torque{};      // 0x170/0x171, bytes 9-10
    bytes::Byte forward_brake{};            // 0x1bc, byte 11
    bytes::Byte four_wheel_drive{};         // 0x1bd, byte 12
    bytes::Byte line_pressure{};            // 0x1be, byte 13
    bytes::Byte temperature_basis{};        // 0x1bf, byte 14

    bool operator==(const TcuParameterReadout&) const = default;
};

// The poll's terminal condition is unresolved -- see the spec. The bytes are
// surfaced for the operator and the bench, never interpreted here.
struct RelearnOutcome
{
    int polls_performed{0};
    bytes::Bytes last_status_frame;

    bool operator==(const RelearnOutcome&) const = default;
};

struct SetParametersOutcome
{
    int frames_written{0};

    bool operator==(const SetParametersOutcome&) const = default;
};

using ServiceFunctionOutcome = std::variant<TcuParameterReadout, RelearnOutcome, SetParametersOutcome>;

struct GateStep
{
    OperatorGateId id;
};

struct CompletedStep
{
    ServiceFunctionOutcome outcome;
};

struct FailedStep
{
    Error error;
};

using ServiceFunctionStep = std::variant<GateStep, CompletedStep, FailedStep>;

} // namespace fastecu::service_functions
```

- [ ] **Step 4: Write the session interface header**

Create `src/backend/service_functions/service_function_session.h`:

```cpp
#pragma once

#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/issm_transport.h"
#include "src/backend/service_functions/service_function_types.h"

namespace fastecu::service_functions
{

// Operator-gated, non-flash TCU routine. The platform owns the thread and the
// dialog; this runs bounded, cancellable I/O and yields when it needs a human.
//
// Deliberately NOT an IFlashExecutor: ConfirmationSpec requires every operator
// answer to be collected before execution begins, and relearn issues an
// instruction mid-sequence (legacy :735). See the design doc.
class ServiceFunctionSession
{
  public:
    virtual ~ServiceFunctionSession() = default;

    // Pure: validates the request and returns the transport configuration this
    // session requires. Performs no I/O, so an unusable request is rejected
    // before the caller touches hardware.
    virtual Result<SsmTransportConfig> transport_setup() const = 0;

    // Runs I/O until the next operator gate, completion, or failure. After a
    // GateStep the caller must submit() an answer before calling resume()
    // again; calling it with a gate outstanding is ErrorKind::Internal.
    virtual ServiceFunctionStep resume(ISsmTransport& transport, IClock& clock,
                                       const ICancellationToken& cancellation, IEventSink& events) = 0;

    virtual void submit(GateResponse response) = 0;
};

} // namespace fastecu::service_functions
```

- [ ] **Step 5: Write the BUILD file**

Create `src/backend/service_functions/BUILD.bazel`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel:gtest_targets.bzl", "fastecu_portable_gtest")

package(default_visibility = [
    "//src/backend:__subpackages__",
    "//src/platform:__subpackages__",
    "//tests:__pkg__",
])

exports_files(
    ["BUILD.bazel"],
    visibility = ["//:__pkg__"],
)

cc_library(
    name = "service_function_types",
    hdrs = ["service_function_types.h"],
    deps = [
        "//src/algorithms/protocol",
        "//src/backend/ports",
    ],
)

cc_library(
    name = "service_function_session",
    hdrs = ["service_function_session.h"],
    deps = [
        ":service_function_types",
        "//src/backend/ports",
        "//src/backend/protocol",
    ],
)

fastecu_portable_gtest(
    name = "service_function_types_test",
    srcs = ["service_function_types_test.cpp"],
    deps = [":service_function_types"],
)
```

- [ ] **Step 6: Register the package in both portable-closure halves**

In `scripts/check-portable-closure.py`, add to `PORTABLE_ROOTS` (keep the dict's existing ordering style, after the `src/backend/protocol/uds` entry):

```python
    ROOT / "src/backend/service_functions": {
        "service_function_types",
        "service_function_session",
    },
```

In the root `BUILD.bazel`, add to the `portable_backend_closure` genquery's target list:

```python
        "//src/backend/service_functions:service_function_types",
        "//src/backend/service_functions:service_function_session",
```

and to the `portable_closure` `py_test`'s `data` list, in its existing alphabetical position:

```python
        "//src/backend/service_functions:BUILD.bazel",
```

- [ ] **Step 7: Write the package README**

Create `src/backend/service_functions/README.md`:

```markdown
# Service functions

Operator-gated, non-flash ECU/TCU routines that exchange bytes over
`ISsmTransport`.

That sentence is the membership rule. A routine belongs here when it needs an
operator to act on the vehicle partway through the sequence, or when it is a
maintenance routine rather than a flash operation — and when it needs nothing
from the transport beyond raw request/response bytes.

Flash operations do not belong here: they build and validate a `FlashPlan`,
collect every confirmation before execution, and run through
`IFlashExecutor`. See `src/backend/flash`.

Today the package holds one family's three operations, ported from
`FlashTcuSubaruDensoSH705xCanOperation`'s `tcuAction` 2, 3 and 4. The design,
including the six defects that port corrects, is in
[the design doc](../../../docs/superpowers/specs/2026-08-31-tcu-service-functions-design.md).
```

- [ ] **Step 8: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/service_functions:service_function_types_test`
Expected: PASS.

- [ ] **Step 9: Prove the closure registration is non-vacuous**

Run: `bazel test --config=release //:portable_closure`
Expected: PASS, and the output names the two new targets.

Now break it deliberately: add `"//src/ui/desktop:desktop_ui"` to `:service_function_types`'s `deps` in the package BUILD file, re-run `bazel test --config=release //:portable_closure`, and confirm it FAILS. Then remove that dep and confirm it passes again. Record the observed failure text in the commit message.

- [ ] **Step 10: Run the full gate**

Run:
```sh
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
prek run --all-files
```
Expected: all pass. `//:legacy_flash_drain` must be unchanged.

- [ ] **Step 11: Commit**

```bash
git add src/backend/service_functions BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat(service-functions): add the portable package skeleton and types"
```

---

### Task 2: The TCU parameter write table

**Files:**
- Create: `src/backend/service_functions/tcu_parameter_table.h`
- Create: `src/backend/service_functions/tcu_parameter_table.cpp`
- Create: `src/backend/service_functions/tcu_parameter_table_test.cpp`
- Modify: `src/backend/service_functions/BUILD.bazel`
- Modify: `scripts/check-portable-closure.py`, root `BUILD.bazel` (add `tcu_parameter_table`)

**Interfaces:**
- Consumes: `service_function_types.h` from Task 1.
- Produces: `fastecu::service_functions::{TcuParameterValues, TcuParameterWrite, kTcuParameterWriteCount, tcu_parameter_writes(const TcuParameterValues&)}`.

This table is the whole of the set-parameters correction. The legacy performs
twelve writes but only the first is well-formed (legacy `:215` reassigns
`output` to the framed array, then `:237-238` and every later pair mutate
indices that now address the SSM length byte and service ID, and re-frame the
already-framed array). Rebuilding each payload from a table is what fixes it.

- [ ] **Step 1: Write the failing test**

Create `src/backend/service_functions/tcu_parameter_table_test.cpp`:

```cpp
#include "src/backend/service_functions/tcu_parameter_table.h"

#include <gtest/gtest.h>

namespace fastecu::service_functions
{
namespace
{

TcuParameterValues sample()
{
    return TcuParameterValues{
        .correction_1to2 = 0x11,
        .correction_2to3 = 0x22,
        .correction_3to4 = 0x33,
        .correction_4to5 = 0x44,
        .correction_forward_brake = 0x55,
        .correction_four_wheel_drive = 0x66,
        .correction_line_pressure = 0x77,
        .temperature_basis = 0x88,
        .torque_correction_awd = 0xBEEF,
    };
}

TEST(TcuParameterTable, WritesTwelveFramesNotNine)
{
    // Ten parameter writes (nine values; AWD torque spans two addresses) plus
    // the two-write commit. legacy :213-479.
    EXPECT_EQ(kTcuParameterWriteCount, 12U);
    EXPECT_EQ(tcu_parameter_writes(sample()).size(), 12U);
}

TEST(TcuParameterTable, PreservesTheLegacyWireOrderNotThePromptOrder)
{
    // Prompts run 1->2, 2->3, 3->4, 4->5, ... (legacy :162-202). Writes run
    // 0x16c = 3->4, 0x16d = 2->3, 0x16e = 1->2, 0x16f = 4->5 (legacy :213-286).
    const auto writes = tcu_parameter_writes(sample());

    EXPECT_EQ(writes[0].address, 0x00016cU);
    EXPECT_EQ(writes[0].value, 0x33); // correction_3to4
    EXPECT_EQ(writes[1].address, 0x00016dU);
    EXPECT_EQ(writes[1].value, 0x22); // correction_2to3
    EXPECT_EQ(writes[2].address, 0x00016eU);
    EXPECT_EQ(writes[2].value, 0x11); // correction_1to2
    EXPECT_EQ(writes[3].address, 0x00016fU);
    EXPECT_EQ(writes[3].value, 0x44); // correction_4to5
}

TEST(TcuParameterTable, SplitsAwdTorqueAcrossTwoAddressesHighFirst)
{
    // legacy :309-334.
    const auto writes = tcu_parameter_writes(sample());

    EXPECT_EQ(writes[4].address, 0x000170U);
    EXPECT_EQ(writes[4].value, 0xBE);
    EXPECT_EQ(writes[5].address, 0x000171U);
    EXPECT_EQ(writes[5].value, 0xEF);
}

TEST(TcuParameterTable, WritesTheRemainingFourCorrections)
{
    // legacy :357-430.
    const auto writes = tcu_parameter_writes(sample());

    EXPECT_EQ(writes[6].address, 0x0001bcU);
    EXPECT_EQ(writes[6].value, 0x55); // forward brake
    EXPECT_EQ(writes[7].address, 0x0001bdU);
    EXPECT_EQ(writes[7].value, 0x66); // 4WD
    EXPECT_EQ(writes[8].address, 0x0001beU);
    EXPECT_EQ(writes[8].value, 0x77); // line pressure
    EXPECT_EQ(writes[9].address, 0x0001bfU);
    EXPECT_EQ(writes[9].value, 0x88); // temperature basis
}

TEST(TcuParameterTable, EndsWithTheTwoWriteCommitToTheSameAddress)
{
    // legacy :453-479 forms B8 00 00 EC 55/AA. Not a parameter and not
    // prompted: a table keyed on the nine prompted values would drop it and
    // leave every write uncommitted.
    const auto writes = tcu_parameter_writes(sample());

    EXPECT_EQ(writes[10].address, 0x0000ecU);
    EXPECT_EQ(writes[10].value, 0x55);
    EXPECT_EQ(writes[11].address, 0x0000ecU);
    EXPECT_EQ(writes[11].value, 0xAA);
}

TEST(TcuParameterTable, CommitValuesAreIndependentOfParameterValues)
{
    TcuParameterValues values = sample();
    values.correction_forward_brake = 0xAA;
    const auto writes = tcu_parameter_writes(values);

    EXPECT_EQ(writes[10].value, 0x55);
    EXPECT_EQ(writes[11].value, 0xAA);
}

} // namespace
} // namespace fastecu::service_functions
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/service_functions:tcu_parameter_table_test`
Expected: FAIL — `tcu_parameter_table.h` does not exist.

- [ ] **Step 3: Write the header**

Create `src/backend/service_functions/tcu_parameter_table.h`:

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"

namespace fastecu::service_functions
{

// The nine values the operator supplies. legacy :162-202 prompts for these
// with bounds 0-255, and 0-65535 for AWD torque -- exactly these types, so the
// bounds are enforced by the value model rather than a runtime range check.
struct TcuParameterValues
{
    bytes::Byte correction_1to2{};             // DC,   written to 0x16e
    bytes::Byte correction_2to3{};             // HLRC, written to 0x16d
    bytes::Byte correction_3to4{};             // IC,   written to 0x16c
    bytes::Byte correction_4to5{};             // FB,   written to 0x16f
    bytes::Byte correction_forward_brake{};    // 0x1bc
    bytes::Byte correction_four_wheel_drive{}; // 0x1bd
    bytes::Byte correction_line_pressure{};    // 0x1be
    bytes::Byte temperature_basis{};           // 0x1bf
    std::uint16_t torque_correction_awd{};     // 0x170 (high) / 0x171 (low)

    bool operator==(const TcuParameterValues&) const = default;
};

// One SSM 0xB8 write-address exchange. Rows are addresses, not parameters:
// AWD torque occupies two, and the trailing commit pair has no parameter.
struct TcuParameterWrite
{
    // 24-bit. Parameter rows are 0x0001xx; commit is 0x0000ec.
    std::uint32_t address;
    bytes::Byte value;

    bool operator==(const TcuParameterWrite&) const = default;
};

inline constexpr std::size_t kTcuParameterWriteCount = 12;

// The twelve writes in legacy wire order. The legacy performs the same twelve
// but only its first frame is well-formed (:215 reassigns `output` to the
// framed array; :237 onward mutate indices that then address the SSM length
// byte and service ID, and re-frame an already-framed buffer). Composing each
// payload from this table is the correction.
std::array<TcuParameterWrite, kTcuParameterWriteCount> tcu_parameter_writes(const TcuParameterValues& values);

} // namespace fastecu::service_functions
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/service_functions/tcu_parameter_table.cpp`:

```cpp
#include "src/backend/service_functions/tcu_parameter_table.h"

namespace fastecu::service_functions
{

std::array<TcuParameterWrite, kTcuParameterWriteCount> tcu_parameter_writes(const TcuParameterValues& values)
{
    return {{
        // legacy :213 -- IC correction, 3->4. First on the wire, third prompted.
        {0x00016c, values.correction_3to4},
        // legacy :237 -- HLRC correction, 2->3.
        {0x00016d, values.correction_2to3},
        // legacy :261 -- DC correction, 1->2.
        {0x00016e, values.correction_1to2},
        // legacy :285 -- FB correction, 4->5.
        {0x00016f, values.correction_4to5},
        // legacy :309 -- AWD clutch torque, high byte.
        {0x000170, static_cast<bytes::Byte>((values.torque_correction_awd >> 8) & 0xff)},
        // legacy :333 -- AWD clutch torque, low byte.
        {0x000171, static_cast<bytes::Byte>(values.torque_correction_awd & 0xff)},
        // legacy :357 -- forward brake pressure correction.
        {0x0001bc, values.correction_forward_brake},
        // legacy :381 -- 4WD pressure correction.
        {0x0001bd, values.correction_four_wheel_drive},
        // legacy :405 -- line pressure correction.
        {0x0001be, values.correction_line_pressure},
        // legacy :429 -- temperature basis for the corrections above.
        {0x0001bf, values.temperature_basis},
        // legacy :453-479 -- B8 00 00 EC 55/AA. Same address, fixed values,
        // no prompt. Without both, every write above stays uncommitted.
        {0x0000ec, 0x55},
        {0x0000ec, 0xaa},
    }};
}

} // namespace fastecu::service_functions
```

- [ ] **Step 5: Add the Bazel targets**

In `src/backend/service_functions/BUILD.bazel`, add:

```python
cc_library(
    name = "tcu_parameter_table",
    srcs = ["tcu_parameter_table.cpp"],
    hdrs = ["tcu_parameter_table.h"],
    deps = ["//src/algorithms/protocol"],
)

fastecu_portable_gtest(
    name = "tcu_parameter_table_test",
    srcs = ["tcu_parameter_table_test.cpp"],
    deps = [":tcu_parameter_table"],
)
```

Add `"tcu_parameter_table"` to this package's `PORTABLE_ROOTS` set in `scripts/check-portable-closure.py`, and `"//src/backend/service_functions:tcu_parameter_table"` to the `portable_backend_closure` genquery in the root `BUILD.bazel`.

- [ ] **Step 6: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/service_functions:tcu_parameter_table_test`
Expected: PASS, 7 tests.

- [ ] **Step 7: Run the full gate and commit**

```sh
bazel test -k --config=release //src/backend/service_functions/... //:portable_closure //:legacy_flash_drain
prek run --all-files
git add src/backend/service_functions BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat(service-functions): add the TCU parameter write table"
```

---

### Task 3: ReadParametersSession

**Files:**
- Create: `src/backend/service_functions/read_parameters_session.h`
- Create: `src/backend/service_functions/read_parameters_session.cpp`
- Create: `src/backend/service_functions/read_parameters_session_test.cpp`
- Modify: `src/backend/service_functions/BUILD.bazel`, `scripts/check-portable-closure.py`, root `BUILD.bazel`

**Interfaces:**
- Consumes: `ServiceFunctionSession`, `SsmTransportConfig`, `TcuParameterReadout` (Task 1).
- Produces: `fastecu::service_functions::ReadParametersSession`, constructed as `ReadParametersSession(std::string protocol)`.

Corrects two defects: the retry loop accepts `0xE8` (legacy `:571-608` accepts
only `0xF8` and the post-check then demands `0xE8`, so it can never succeed),
and the response must be at least 15 bytes (legacy guards `> 10` but decodes
through byte 14).

- [ ] **Step 1: Write the failing test**

Create `src/backend/service_functions/read_parameters_session_test.cpp`:

```cpp
#include "src/backend/service_functions/read_parameters_session.h"

#include <gtest/gtest.h>

#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

namespace fastecu::service_functions
{
namespace
{

// legacy :534-570 -- 0xA8 read of ten addresses behind the 0x7E1 envelope.
const bytes::Bytes kRequest{
    0x00, 0x00, 0x07, 0xe1, 0xa8, 0x00,
    0x00, 0x01, 0x6c, 0x00, 0x01, 0x6d, 0x00, 0x01, 0x6e, 0x00, 0x01, 0x6f,
    0x00, 0x01, 0x70, 0x00, 0x01, 0x71, 0x00, 0x01, 0xbc, 0x00, 0x01, 0xbd,
    0x00, 0x01, 0xbe, 0x00, 0x01, 0xbf,
};

// Four envelope bytes, 0xE8, then the ten data bytes at 5..14.
bytes::Bytes goodReply()
{
    return {0x00, 0x00, 0x07, 0xe9, 0xe8, 0x11, 0x22, 0x33, 0x44, 0xbe, 0xef, 0x55, 0x66, 0x77, 0x88};
}

struct Fixture
{
    ScriptedSsmTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    NullEventSink events;
    ReadParametersSession session{"sub_tcu_denso_sh7058_can"};
};

TEST(ReadParametersSession, RequiresTheIso15765TcuPair)
{
    const Fixture fixture;
    const auto setup = fixture.session.transport_setup();
    ASSERT_TRUE(setup.has_value());
    EXPECT_EQ(setup->framing, SsmTransportConfig::Framing::Iso15765);
    EXPECT_EQ(setup->bitrate_or_baud, 500000);
    EXPECT_EQ(setup->request_id, 0x7e1U);
    EXPECT_EQ(setup->response_id, 0x7e9U);
}

TEST(ReadParametersSession, RejectsAnUnknownProtocolBeforeAnyIo)
{
    const ReadParametersSession session{"sub_ecu_denso_sh7058_can"};
    const auto setup = session.transport_setup();
    ASSERT_FALSE(setup.has_value());
    EXPECT_EQ(setup.error().kind, ErrorKind::Unsupported);
}

TEST(ReadParametersSession, DecodesTheNineValuesFromBytesFiveToFourteen)
{
    Fixture fixture;
    fixture.transport.expectWrite(kRequest);
    fixture.transport.queueRead(goodReply());

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);

    ASSERT_TRUE(std::holds_alternative<CompletedStep>(step));
    const auto& readout = std::get<TcuParameterReadout>(std::get<CompletedStep>(step).outcome);
    EXPECT_EQ(readout.input_clutch, 0x11);
    EXPECT_EQ(readout.high_low_reverse_clutch, 0x22);
    EXPECT_EQ(readout.direct_clutch, 0x33);
    EXPECT_EQ(readout.front_brake, 0x44);
    EXPECT_EQ(readout.awd_clutch_torque, 0xbeefU);
    EXPECT_EQ(readout.forward_brake, 0x55);
    EXPECT_EQ(readout.four_wheel_drive, 0x66);
    EXPECT_EQ(readout.line_pressure, 0x77);
    EXPECT_EQ(readout.temperature_basis, 0x88);
    EXPECT_TRUE(fixture.transport.ok());
    EXPECT_TRUE(fixture.transport.scriptConsumed());
}

TEST(ReadParametersSession, AcceptsE8WhichTheLegacyRetryLoopNeverCould)
{
    // legacy :571-608 sets responseOK only on 0xF8 and then demands 0xE8, so
    // the legacy always returns STATUS_ERROR. This is the corrected path.
    Fixture fixture;
    fixture.transport.expectWrite(kRequest);
    fixture.transport.queueRead(goodReply());

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    EXPECT_TRUE(std::holds_alternative<CompletedStep>(step));
}

TEST(ReadParametersSession, RetriesUpToSixTimes)
{
    // legacy :571 -- while (try_count < 6 && !responseOK).
    Fixture fixture;
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        fixture.transport.expectWrite(kRequest);
    }
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        fixture.transport.queue_no_frame();
    }
    fixture.transport.queueRead(goodReply());

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    EXPECT_TRUE(std::holds_alternative<CompletedStep>(step));
    EXPECT_TRUE(fixture.transport.scriptConsumed());
}

TEST(ReadParametersSession, TimesOutWhenAllSixAttemptsAreSilent)
{
    Fixture fixture;
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        fixture.transport.expectWrite(kRequest);
        fixture.transport.queue_no_frame();
    }

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Timeout);
}

TEST(ReadParametersSession, RejectsAFrameShorterThanFifteenBytes)
{
    // legacy :592 guards on length() > 10 but :624 indexes byte 14.
    Fixture fixture;
    fixture.transport.expectWrite(kRequest);
    fixture.transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xe9, 0xe8, 0x11, 0x22, 0x33, 0x44, 0xbe, 0xef, 0x55});

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::BadResponse);
}

TEST(ReadParametersSession, ReportsANegativeResponseAsBadResponse)
{
    Fixture fixture;
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        fixture.transport.expectWrite(kRequest);
        fixture.transport.queueRead(
            bytes::Bytes{0x00, 0x00, 0x07, 0xe9, 0x7f, 0xa8, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    }

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::BadResponse);
}

TEST(ReadParametersSession, ReportsADroppedTransportAsDisconnected)
{
    Fixture fixture;
    fixture.transport.expectWrite(kRequest);
    fixture.transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Disconnected);
}

TEST(ReadParametersSession, ObservesCancellation)
{
    Fixture fixture;
    fixture.cancellation.set_cancelled(true);

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Cancelled);
}

TEST(ReadParametersSession, SubmitIsInternalBecauseItHasNoGates)
{
    Fixture fixture;
    fixture.session.submit(GateResponse::Accept);
    fixture.transport.expectWrite(kRequest);
    fixture.transport.queueRead(goodReply());

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Internal);
}

} // namespace
} // namespace fastecu::service_functions
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/service_functions:read_parameters_session_test`
Expected: FAIL — `read_parameters_session.h` does not exist.

- [ ] **Step 3: Write the header**

Create `src/backend/service_functions/read_parameters_session.h`:

```cpp
#pragma once

#include <string>

#include "src/backend/service_functions/service_function_session.h"

namespace fastecu::service_functions
{

// Portable equivalent of FlashTcuSubaruDensoSH705xCanOperation::
// tcu_readparam_subaru_ssm (legacy :517-632), which cannot succeed: its retry
// loop accepts only 0xF8 while the post-loop check demands 0xE8. This session
// accepts 0xE8, the correct positive response to the 0xA8 request it sends,
// and requires the 15 bytes the decode actually indexes.
//
// Has no operator gates: submit() is a contract violation and makes the next
// resume() fail with ErrorKind::Internal.
class ReadParametersSession final : public ServiceFunctionSession
{
  public:
    explicit ReadParametersSession(std::string protocol);

    Result<SsmTransportConfig> transport_setup() const override;
    ServiceFunctionStep resume(ISsmTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               IEventSink& events) override;
    void submit(GateResponse response) override;

  private:
    std::string protocol_;
    bool misused_{false};
};

} // namespace fastecu::service_functions
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/service_functions/read_parameters_session.cpp`:

```cpp
#include "src/backend/service_functions/read_parameters_session.h"

#include <array>
#include <utility>

namespace fastecu::service_functions
{
namespace
{

constexpr int kAttempts = 6;         // legacy :571
constexpr int kReadTimeoutMs = 200;  // serial_read_short_timeout, legacy header :62
constexpr std::size_t kMinFrameSize = 15; // bytes 5..14 are decoded; legacy guards > 10
constexpr bytes::Byte kPositiveResponse = 0xe8;

// legacy :540-570 -- ten addresses, in this order.
constexpr std::array<std::uint16_t, 10> kAddresses{0x16c, 0x16d, 0x16e, 0x16f, 0x170,
                                                   0x171, 0x1bc, 0x1bd, 0x1be, 0x1bf};

bytes::Bytes buildRequest()
{
    // legacy :534-539 -- 0x7E1 envelope, then SID 0xA8 and the "one time only"
    // response-mode byte.
    bytes::Bytes request{0x00, 0x00, 0x07, 0xe1, 0xa8, 0x00};
    for (const std::uint16_t address : kAddresses)
    {
        bytes::appendU24Be(request, address);
    }
    return request;
}

TcuParameterReadout decode(bytes::ByteView frame)
{
    // legacy :610-624 -- nine values across response bytes 5..14.
    return TcuParameterReadout{
        .input_clutch = frame[5],
        .high_low_reverse_clutch = frame[6],
        .direct_clutch = frame[7],
        .front_brake = frame[8],
        .awd_clutch_torque = bytes::readU16Be(frame, 9),
        .forward_brake = frame[11],
        .four_wheel_drive = frame[12],
        .line_pressure = frame[13],
        .temperature_basis = frame[14],
    };
}

} // namespace

ReadParametersSession::ReadParametersSession(std::string protocol) : protocol_(std::move(protocol))
{
}

Result<SsmTransportConfig> ReadParametersSession::transport_setup() const
{
    if (protocol_ != "sub_tcu_denso_sh7055_can" && protocol_ != "sub_tcu_denso_sh7058_can")
    {
        return fail(ErrorKind::Unsupported, "not a Subaru Denso SH705x TCU protocol: " + protocol_);
    }
    // legacy :70 -- configureIso15765Can(serial, "500000", 0x7E1, 0x7E9).
    return SsmTransportConfig{};
}

void ReadParametersSession::submit(GateResponse)
{
    misused_ = true;
}

ServiceFunctionStep ReadParametersSession::resume(ISsmTransport& transport, IClock&,
                                                  const ICancellationToken& cancellation, IEventSink& events)
{
    if (misused_)
    {
        return FailedStep{Error{ErrorKind::Internal, "read parameters has no operator gate to answer"}};
    }
    if (cancellation.cancelled())
    {
        return FailedStep{Error{ErrorKind::Cancelled, "cancelled before reading TCU parameters"}};
    }

    events.log(LogLevel::Info, "Reading TCU parameters...");

    const bytes::Bytes request = buildRequest();
    bytes::Bytes frame;

    for (int attempt = 0; attempt < kAttempts; ++attempt)
    {
        if (const auto written = transport.write(request); !written.has_value())
        {
            return FailedStep{written.error()};
        }

        const auto received = transport.read(kReadTimeoutMs, cancellation);
        if (!received.has_value())
        {
            return FailedStep{received.error()};
        }
        if (!received->has_value())
        {
            continue; // deadline with no frame; legacy simply retries
        }

        frame = **received;
        // legacy accepts only 0xF8 here and then demands 0xE8 below, so it can
        // never succeed. 0xE8 is the positive response to the 0xA8 sent above.
        if (frame.size() > 4 && frame[4] == kPositiveResponse)
        {
            if (frame.size() < kMinFrameSize)
            {
                return FailedStep{Error{ErrorKind::BadResponse, "TCU parameter frame shorter than 15 bytes"}};
            }
            return CompletedStep{decode(frame)};
        }
    }

    if (frame.empty())
    {
        return FailedStep{Error{ErrorKind::Timeout, "no response to the TCU parameter read after 6 attempts"}};
    }
    return FailedStep{Error{ErrorKind::BadResponse, "TCU rejected the parameter read: " + bytes::toHex(frame)}};
}

} // namespace fastecu::service_functions
```

- [ ] **Step 5: Add the Bazel targets and register the closure**

In `src/backend/service_functions/BUILD.bazel`:

```python
cc_library(
    name = "read_parameters_session",
    srcs = ["read_parameters_session.cpp"],
    hdrs = ["read_parameters_session.h"],
    deps = [":service_function_session"],
)

fastecu_portable_gtest(
    name = "read_parameters_session_test",
    srcs = ["read_parameters_session_test.cpp"],
    deps = [
        ":read_parameters_session",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/protocol/testing:scripted_transports",
    ],
)
```

Add `"read_parameters_session"` to `PORTABLE_ROOTS` and
`"//src/backend/service_functions:read_parameters_session"` to the genquery.

- [ ] **Step 6: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/service_functions:read_parameters_session_test`
Expected: PASS, 11 tests.

- [ ] **Step 7: Run the full gate and commit**

```sh
bazel test -k --config=release //src/backend/service_functions/... //:portable_closure //:legacy_flash_drain
prek run --all-files
git add src/backend/service_functions BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat(service-functions): port TCU read-parameters and fix its response check"
```

---

### Task 4: SetParametersSession

**Files:**
- Create: `src/backend/service_functions/set_parameters_session.h`
- Create: `src/backend/service_functions/set_parameters_session.cpp`
- Create: `src/backend/service_functions/set_parameters_session_test.cpp`
- Modify: `src/backend/service_functions/BUILD.bazel`, `scripts/check-portable-closure.py`, root `BUILD.bazel`

**Interfaces:**
- Consumes: `ServiceFunctionSession` (Task 1), `TcuParameterValues` / `tcu_parameter_writes` (Task 2).
- Produces: `fastecu::service_functions::SetParametersSession`, constructed as `SetParametersSession(std::string protocol, TcuParameterValues values)`.

This is the K-Line session: legacy `:141-152` switches the whole session to
ISO14230 at 4800 baud with tester `0xF0` / target `0x18` before touching
anything, discarding the ISO-15765 port `execute()` opened.

**Safety amendment:** each table row gets exactly one exchange. The direct
legacy flow has no retry loop and returns on its first silence/rejection;
repeating a live parameter or commit write has no idempotency authority. Tests
derive the complete ten-byte wire frames as literals, independently of both
the table and `SsmProtocol::addHeader`, including commit frames
`80 18 F0 05 B8 00 00 EC 55 86` and
`80 18 F0 05 B8 00 00 EC AA DB`.

- [ ] **Step 1: Write the failing test**

Create `src/backend/service_functions/set_parameters_session_test.cpp`:

```cpp
#include "src/backend/service_functions/set_parameters_session.h"

#include <gtest/gtest.h>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

namespace fastecu::service_functions
{
namespace
{

TcuParameterValues sample()
{
    return TcuParameterValues{
        .correction_1to2 = 0x11,
        .correction_2to3 = 0x22,
        .correction_3to4 = 0x33,
        .correction_4to5 = 0x44,
        .correction_forward_brake = 0x55,
        .correction_four_wheel_drive = 0x66,
        .correction_line_pressure = 0x77,
        .temperature_basis = 0x88,
        .torque_correction_awd = 0xbeef,
    };
}

// legacy :210-215 -- payload 0xB8 + 24-bit address + value, framed once.
bytes::Bytes framed(std::uint32_t address, bytes::Byte value)
{
    bytes::Bytes payload{0xb8};
    bytes::appendU24Be(payload, address);
    payload.push_back(value);
    return SsmProtocol::addHeader(payload, 0xf0, 0x18);
}

bytes::Bytes ack()
{
    // Positive response 0xF8 at index 4, behind the SSM header.
    return {0x80, 0xf0, 0x18, 0x02, 0xf8, 0x00, 0x00};
}

struct Fixture
{
    ScriptedSsmTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    NullEventSink events;
    SetParametersSession session{"sub_tcu_denso_sh7058_can", sample()};
};

void scriptAllTwelve(ScriptedSsmTransport& transport)
{
    for (const auto& write : tcu_parameter_writes(sample()))
    {
        transport.expectWrite(framed(write.address, write.value));
        transport.queueRead(ack());
    }
}

TEST(SetParametersSession, RequiresTheKlineConfigurationNotTheCanOne)
{
    // legacy :141-152 -- K-Line, 4800 baud, tester 0xF0, target 0x18, and the
    // driver's ISO14230 auto-header off because the session frames its own.
    const Fixture fixture;
    const auto setup = fixture.session.transport_setup();
    ASSERT_TRUE(setup.has_value());
    EXPECT_EQ(setup->framing, SsmTransportConfig::Framing::Kline14230);
    EXPECT_EQ(setup->bitrate_or_baud, 4800);
    EXPECT_EQ(setup->tester_id, 0xf0);
    EXPECT_EQ(setup->target_id, 0x18);
    EXPECT_FALSE(setup->add_iso14230_header);
}

TEST(SetParametersSession, RejectsAnUnknownProtocolBeforeAnyIo)
{
    const SetParametersSession session{"sub_ecu_denso_sh7058_can", sample()};
    const auto setup = session.transport_setup();
    ASSERT_FALSE(setup.has_value());
    EXPECT_EQ(setup.error().kind, ErrorKind::Unsupported);
}

TEST(SetParametersSession, WritesAllTwelveFramesEachFramedExactlyOnce)
{
    // The legacy corrupts every frame after the first (:215 reassigns `output`
    // to the framed array; :237 onward mutate the length byte and service ID
    // and re-frame it), so it aborts on write 2 leaving the TCU half set.
    Fixture fixture;
    scriptAllTwelve(fixture.transport);

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);

    ASSERT_TRUE(std::holds_alternative<CompletedStep>(step));
    EXPECT_EQ(std::get<SetParametersOutcome>(std::get<CompletedStep>(step).outcome).frames_written, 12);
    EXPECT_TRUE(fixture.transport.ok());
    EXPECT_TRUE(fixture.transport.scriptConsumed());
}

TEST(SetParametersSession, FirstFrameMatchesTheOneFrameTheLegacyGetsRight)
{
    // legacy :210-215 is the only well-formed legacy frame; ours must equal it
    // byte for byte: 80 18 F0 05 B8 00 01 6C 33 <sum8>.
    const bytes::Bytes first = framed(0x00016c, 0x33);
    ASSERT_EQ(first.size(), 10U);
    EXPECT_EQ(first[0], 0x80);
    EXPECT_EQ(first[1], 0x18);
    EXPECT_EQ(first[2], 0xf0);
    EXPECT_EQ(first[3], 0x05);
    EXPECT_EQ(first[4], 0xb8);
    EXPECT_EQ(first[8], 0x33);

    Fixture fixture;
    scriptAllTwelve(fixture.transport);
    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    EXPECT_TRUE(std::holds_alternative<CompletedStep>(step));
}

TEST(SetParametersSession, StopsAtTheFirstNonPositiveResponse)
{
    // legacy :227 returns STATUS_ERROR without commenting the return out.
    Fixture fixture;
    const auto writes = tcu_parameter_writes(sample());
    fixture.transport.expectWrite(framed(writes[0].address, writes[0].value));
    fixture.transport.queueRead(ack());
    fixture.transport.expectWrite(framed(writes[1].address, writes[1].value));
    fixture.transport.queueRead(bytes::Bytes{0x80, 0xf0, 0x18, 0x02, 0x7f, 0xb8, 0x11});

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::BadResponse);
}

TEST(SetParametersSession, TreatsASilentReadAsTimeout)
{
    Fixture fixture;
    const auto writes = tcu_parameter_writes(sample());
    fixture.transport.expectWrite(framed(writes[0].address, writes[0].value));
    fixture.transport.queue_no_frame();

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Timeout);
}

TEST(SetParametersSession, ReportsADroppedTransportAsDisconnected)
{
    Fixture fixture;
    const auto writes = tcu_parameter_writes(sample());
    fixture.transport.expectWrite(framed(writes[0].address, writes[0].value));
    fixture.transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Disconnected);
}

TEST(SetParametersSession, ObservesCancellationBetweenWrites)
{
    // resume() polls the token twice per write: once at the top of the loop
    // and once inside ScriptedSsmTransport::read. Tripping on the third check
    // therefore lands after write 1 completes and before write 2 is sent.
    Fixture fixture;
    const auto writes = tcu_parameter_writes(sample());
    fixture.transport.expectWrite(framed(writes[0].address, writes[0].value));
    fixture.transport.queueRead(ack());
    fixture.cancellation.cancel_on_check(3);

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Cancelled);
    EXPECT_TRUE(fixture.transport.scriptConsumed()); // exactly one write went out
}

TEST(SetParametersSession, SubmitIsInternalBecauseItHasNoGates)
{
    Fixture fixture;
    fixture.session.submit(GateResponse::Accept);

    const auto step = fixture.session.resume(fixture.transport, fixture.clock, fixture.cancellation, fixture.events);
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Internal);
}

} // namespace
} // namespace fastecu::service_functions
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/service_functions:set_parameters_session_test`
Expected: FAIL — `set_parameters_session.h` does not exist.

- [ ] **Step 3: Write the header**

Create `src/backend/service_functions/set_parameters_session.h`:

```cpp
#pragma once

#include <string>

#include "src/backend/service_functions/service_function_session.h"
#include "src/backend/service_functions/tcu_parameter_table.h"

namespace fastecu::service_functions
{

// Portable equivalent of FlashTcuSubaruDensoSH705xCanOperation::
// tcu_setparam_subaru_ssm (legacy :135-517). The legacy writes twelve frames
// but only its first is well-formed, so it aborts on the second and leaves the
// TCU with one correction applied and eleven not, uncommitted. This session
// composes and frames each write from tcu_parameter_writes().
//
// Runs on K-Line, not CAN: legacy :141-152 switches the session to ISO14230 at
// 4800 baud before any parameter I/O.
class SetParametersSession final : public ServiceFunctionSession
{
  public:
    SetParametersSession(std::string protocol, TcuParameterValues values);

    Result<SsmTransportConfig> transport_setup() const override;
    ServiceFunctionStep resume(ISsmTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               IEventSink& events) override;
    void submit(GateResponse response) override;

  private:
    std::string protocol_;
    TcuParameterValues values_;
    bool misused_{false};
};

} // namespace fastecu::service_functions
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/service_functions/set_parameters_session.cpp`:

```cpp
#include "src/backend/service_functions/set_parameters_session.h"

#include <utility>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::service_functions
{
namespace
{

constexpr int kReadTimeoutMs = 500;  // receive_timeout, legacy header :59
constexpr bytes::Byte kTesterId = 0xf0; // legacy :151
constexpr bytes::Byte kTargetId = 0x18; // legacy :152
constexpr bytes::Byte kPositiveResponse = 0xf8;

bytes::Bytes frameFor(const TcuParameterWrite& write)
{
    // legacy :210-215 -- SID 0xB8, 24-bit address, value, framed exactly once.
    bytes::Bytes payload{0xb8};
    bytes::appendU24Be(payload, write.address);
    payload.push_back(write.value);
    return SsmProtocol::addHeader(payload, kTesterId, kTargetId);
}

} // namespace

SetParametersSession::SetParametersSession(std::string protocol, TcuParameterValues values)
    : protocol_(std::move(protocol)), values_(values)
{
}

Result<SsmTransportConfig> SetParametersSession::transport_setup() const
{
    if (protocol_ != "sub_tcu_denso_sh7055_can" && protocol_ != "sub_tcu_denso_sh7058_can")
    {
        return fail(ErrorKind::Unsupported, "not a Subaru Denso SH705x TCU protocol: " + protocol_);
    }
    // legacy :141-152 -- "CAN 0xb8 command is disabled, so switch to K-Line comms".
    return SsmTransportConfig{
        .framing = SsmTransportConfig::Framing::Kline14230,
        .bitrate_or_baud = 4800,
        .request_id = 0,
        .response_id = 0,
        .tester_id = kTesterId,
        .target_id = kTargetId,
        .add_iso14230_header = false,
    };
}

void SetParametersSession::submit(GateResponse)
{
    misused_ = true;
}

ServiceFunctionStep SetParametersSession::resume(ISsmTransport& transport, IClock&,
                                                 const ICancellationToken& cancellation, IEventSink& events)
{
    if (misused_)
    {
        return FailedStep{Error{ErrorKind::Internal, "set parameters has no operator gate to answer"}};
    }

    events.log(LogLevel::Info, "Setting TCU parameters...");

    const auto writes = tcu_parameter_writes(values_);
    int written_count = 0;

    for (const auto& write : writes)
    {
        if (cancellation.cancelled())
        {
            return FailedStep{Error{ErrorKind::Cancelled, "cancelled while setting TCU parameters"}};
        }

        const bytes::Bytes frame = frameFor(write);
        if (const auto sent = transport.write(frame); !sent.has_value())
        {
            return FailedStep{sent.error()};
        }

        const auto received = transport.read(kReadTimeoutMs, cancellation);
        if (!received.has_value())
        {
            return FailedStep{received.error()};
        }
        if (!received->has_value())
        {
            return FailedStep{Error{ErrorKind::Timeout, "no response to TCU parameter write"}};
        }

        // legacy :219-236 -- this check is NOT commented out, unlike relearn's.
        const bytes::Bytes& reply = **received;
        if (reply.size() <= 4 || reply[4] != kPositiveResponse)
        {
            return FailedStep{Error{ErrorKind::BadResponse, "TCU rejected a parameter write: " + bytes::toHex(reply)}};
        }

        ++written_count;
        events.progress(written_count, static_cast<int>(writes.size()));
    }

    return CompletedStep{SetParametersOutcome{.frames_written = written_count}};
}

} // namespace fastecu::service_functions
```

- [ ] **Step 5: Add the Bazel targets and register the closure**

```python
cc_library(
    name = "set_parameters_session",
    srcs = ["set_parameters_session.cpp"],
    hdrs = ["set_parameters_session.h"],
    deps = [
        ":service_function_session",
        ":tcu_parameter_table",
        "//src/algorithms/protocol/ssm",
    ],
)

fastecu_portable_gtest(
    name = "set_parameters_session_test",
    srcs = ["set_parameters_session_test.cpp"],
    deps = [
        ":set_parameters_session",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/protocol/testing:scripted_transports",
    ],
)
```

Add `"set_parameters_session"` to `PORTABLE_ROOTS` and the matching label to the genquery.

- [ ] **Step 6: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/service_functions:set_parameters_session_test`
Expected: PASS, 9 tests.

- [ ] **Step 7: Run the full gate and commit**

```sh
bazel test -k --config=release //src/backend/service_functions/... //:portable_closure //:legacy_flash_drain
prek run --all-files
git add src/backend/service_functions BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat(service-functions): port TCU set-parameters and fix its frame corruption"
```

---

### Task 5: RelearnSession

**Files:**
- Create: `src/backend/service_functions/relearn_session.h`
- Create: `src/backend/service_functions/relearn_session.cpp`
- Create: `src/backend/service_functions/relearn_session_test.cpp`
- Modify: `src/backend/service_functions/BUILD.bazel`, `scripts/check-portable-closure.py`, root `BUILD.bazel`

**Interfaces:**
- Consumes: `ServiceFunctionSession` (Task 1).
- Produces: `fastecu::service_functions::RelearnSession`, constructed as `RelearnSession(std::string protocol)`.

The only session with gates, and the reason the package exists. Corrects three
defects: the unconditional final `return STATUS_ERROR` (legacy `:786`), the
poll waiting for `0xF8` after an `0xA8` request, and the poll frame written
three bytes past the end of a 9-byte buffer (legacy `:740-747`).

- [ ] **Step 1: Write the failing test**

Create `src/backend/service_functions/relearn_session_test.cpp`:

```cpp
#include "src/backend/service_functions/relearn_session.h"

#include <gtest/gtest.h>

#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

namespace fastecu::service_functions
{
namespace
{

// legacy :655-663 -- 0x7E1 envelope, SID 0xB8, address 0x1FC, value 0x01.
const bytes::Bytes kStepOne{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfc, 0x01};
// legacy :699-700 -- same frame with address 0x1FD and value 0x09.
const bytes::Bytes kStepTwo{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfd, 0x09};
// legacy :740-747 intends this 12-byte 0xA8 read of 0x1FC and 0x1FD, but
// writes indices 9-11 past the end of the 9-byte step-two buffer.
const bytes::Bytes kPoll{0x00, 0x00, 0x07, 0xe1, 0xa8, 0x00, 0x00, 0x01, 0xfc, 0x00, 0x01, 0xfd};

bytes::Bytes writeAck()
{
    return {0x00, 0x00, 0x07, 0xe9, 0xf8, 0x00};
}

bytes::Bytes pollReply(bytes::Byte first, bytes::Byte second)
{
    return {0x00, 0x00, 0x07, 0xe9, 0xe8, first, second};
}

struct Fixture
{
    ScriptedSsmTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    NullEventSink events;
    RelearnSession session{"sub_tcu_denso_sh7058_can"};

    ServiceFunctionStep step()
    {
        return session.resume(transport, clock, cancellation, events);
    }
};

TEST(RelearnSession, RequiresTheIso15765TcuPair)
{
    const Fixture fixture;
    const auto setup = fixture.session.transport_setup();
    ASSERT_TRUE(setup.has_value());
    EXPECT_EQ(setup->framing, SsmTransportConfig::Framing::Iso15765);
    EXPECT_EQ(setup->request_id, 0x7e1U);
}

TEST(RelearnSession, RejectsAnUnknownProtocolBeforeAnyIo)
{
    const RelearnSession session{"sub_ecu_denso_sh7058_can"};
    const auto setup = session.transport_setup();
    ASSERT_FALSE(setup.has_value());
    EXPECT_EQ(setup.error().kind, ErrorKind::Unsupported);
}

TEST(RelearnSession, AsksForTheStaticSetupGateBeforeAnyIo)
{
    Fixture fixture;
    const auto step = fixture.step();

    ASSERT_TRUE(std::holds_alternative<GateStep>(step));
    EXPECT_EQ(std::get<GateStep>(step).id, OperatorGateId::RelearnStaticSetup);
    EXPECT_TRUE(fixture.transport.scriptConsumed()); // nothing written yet
}

TEST(RelearnSession, AsksForTheEngineRunningGateOnlyAfterStepTwoIsAccepted)
{
    // This ordering is the whole reason the package exists: legacy :735 issues
    // the instruction after the TCU accepts step two, so it cannot be
    // pre-collected the way ConfirmationSpec requires.
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queueRead(writeAck());
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<GateStep>(step));
    EXPECT_EQ(std::get<GateStep>(step).id, OperatorGateId::RelearnEngineRunning);
    EXPECT_TRUE(fixture.transport.ok());
    EXPECT_TRUE(fixture.transport.scriptConsumed());
}

TEST(RelearnSession, PollsWithTheTwelveByteFrameTheLegacyCannotBuild)
{
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queueRead(writeAck());
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    for (int poll = 0; poll < 200; ++poll)
    {
        fixture.transport.expectWrite(kPoll);
        fixture.transport.queueRead(pollReply(0x01, 0x02));
    }

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<CompletedStep>(step));
    const auto& outcome = std::get<RelearnOutcome>(std::get<CompletedStep>(step).outcome);
    EXPECT_EQ(outcome.polls_performed, 200);
    EXPECT_EQ(outcome.last_status_frame, pollReply(0x01, 0x02));
    EXPECT_TRUE(fixture.transport.ok());
}

TEST(RelearnSession, ReportsSuccessWhereTheLegacyAlwaysReportedFailure)
{
    // legacy :786 -- the function's last statement is return STATUS_ERROR.
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);
    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queueRead(writeAck());
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);
    for (int poll = 0; poll < 200; ++poll)
    {
        fixture.transport.expectWrite(kPoll);
        fixture.transport.queueRead(pollReply(0x00, 0x00));
    }

    EXPECT_TRUE(std::holds_alternative<CompletedStep>(fixture.step()));
}

TEST(RelearnSession, ToleratesABadStepOneResponseAndContinues)
{
    // legacy :688 and :694 -- both returns are commented out, so the legacy
    // logs and proceeds. Tolerance is real behavior and is preserved.
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    for (int attempt = 0; attempt < 6; ++attempt)
    {
        fixture.transport.expectWrite(kStepOne);
        fixture.transport.queueRead(bytes::Bytes{0x00, 0x00, 0x07, 0xe9, 0x7f, 0xb8, 0x11});
    }
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<GateStep>(step));
    EXPECT_EQ(std::get<GateStep>(step).id, OperatorGateId::RelearnEngineRunning);
}

TEST(RelearnSession, RetriesEachWriteStepUpToSixTimes)
{
    // legacy :702 -- while (try_count < 6 && !responseOK).
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        fixture.transport.expectWrite(kStepOne);
        fixture.transport.queue_no_frame();
    }
    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queueRead(writeAck());
    fixture.transport.expectWrite(kStepTwo);
    fixture.transport.queueRead(writeAck());

    EXPECT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    EXPECT_TRUE(fixture.transport.scriptConsumed());
}

TEST(RelearnSession, ADeclinedGateEndsTheSessionAsCancelledNotFailed)
{
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Decline);

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Cancelled);
    EXPECT_TRUE(fixture.transport.scriptConsumed()); // declined before any I/O
}

TEST(RelearnSession, ResumeWithAGateOutstandingIsInternal)
{
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));

    const auto step = fixture.step(); // no submit() in between
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Internal);
}

TEST(RelearnSession, ReportsADroppedTransportAsDisconnected)
{
    Fixture fixture;
    ASSERT_TRUE(std::holds_alternative<GateStep>(fixture.step()));
    fixture.session.submit(GateResponse::Accept);
    fixture.transport.expectWrite(kStepOne);
    fixture.transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Disconnected);
}

TEST(RelearnSession, ObservesCancellationAtTheFirstGate)
{
    Fixture fixture;
    fixture.cancellation.set_cancelled(true);

    const auto step = fixture.step();
    ASSERT_TRUE(std::holds_alternative<FailedStep>(step));
    EXPECT_EQ(std::get<FailedStep>(step).error.kind, ErrorKind::Cancelled);
}

} // namespace
} // namespace fastecu::service_functions
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/service_functions:relearn_session_test`
Expected: FAIL — `relearn_session.h` does not exist.

- [ ] **Step 3: Write the header**

Create `src/backend/service_functions/relearn_session.h`:

```cpp
#pragma once

#include <string>

#include "src/backend/service_functions/service_function_session.h"

namespace fastecu::service_functions
{

// Portable equivalent of FlashTcuSubaruDensoSH705xCanOperation::
// tcu_relearn_subaru_ssm (legacy :632-790), which always reports failure: its
// last statement is an unconditional return STATUS_ERROR (:786).
//
// Three corrections, all named in the flash qualification matrix:
//   1. completion reports success;
//   2. the status poll accepts 0xE8, the positive response to the 0xA8 it
//      sends, instead of the unreachable 0xF8;
//   3. the poll frame is composed directly. Legacy :740-747 rewrites the
//      9-byte step-two buffer at indices 9-11, three bytes past its end, so
//      the second status address never reaches the wire.
//
// The poll's terminal condition is deliberately NOT invented: the bound stays
// at 200 iterations and the last frame is surfaced for the bench.
class RelearnSession final : public ServiceFunctionSession
{
  public:
    explicit RelearnSession(std::string protocol);

    Result<SsmTransportConfig> transport_setup() const override;
    ServiceFunctionStep resume(ISsmTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               IEventSink& events) override;
    void submit(GateResponse response) override;

  private:
    enum class Stage
    {
        AwaitStaticSetupGate,
        WriteSteps,
        AwaitEngineRunningGate,
        Poll,
        Done,
    };

    std::string protocol_;
    Stage stage_{Stage::AwaitStaticSetupGate};
    bool gate_outstanding_{false};
    bool declined_{false};
};

} // namespace fastecu::service_functions
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/service_functions/relearn_session.cpp`:

```cpp
#include "src/backend/service_functions/relearn_session.h"

#include <utility>

namespace fastecu::service_functions
{
namespace
{

constexpr int kWriteAttempts = 6;    // legacy :702
constexpr int kPollIterations = 200; // legacy :748
constexpr int kReadTimeoutMs = 200;  // serial_read_short_timeout, legacy header :62
constexpr bytes::Byte kWriteAck = 0xf8;
constexpr bytes::Byte kReadAck = 0xe8;

// legacy :655-663.
const bytes::Bytes kStepOne{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfc, 0x01};
// legacy :699-700.
const bytes::Bytes kStepTwo{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfd, 0x09};
// legacy :740-747 intends this; see the header for why it cannot build it.
const bytes::Bytes kPoll{0x00, 0x00, 0x07, 0xe1, 0xa8, 0x00, 0x00, 0x01, 0xfc, 0x00, 0x01, 0xfd};

// Sends `frame` up to six times, accepting `expected` at index 4. Returns the
// last frame seen. A bad or absent response is tolerated by the caller, which
// is what legacy :688/:694/:722/:733 do with their commented-out returns.
Result<bytes::Bytes> exchangeTolerantly(ISsmTransport& transport, const ICancellationToken& cancellation,
                                        bytes::ByteView frame, bytes::Byte expected)
{
    bytes::Bytes last;
    for (int attempt = 0; attempt < kWriteAttempts; ++attempt)
    {
        if (const auto sent = transport.write(frame); !sent.has_value())
        {
            return std::unexpected(sent.error());
        }

        const auto received = transport.read(kReadTimeoutMs, cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            continue;
        }

        last = **received;
        if (last.size() > 4 && last[4] == expected)
        {
            return last;
        }
    }
    return last;
}

} // namespace

RelearnSession::RelearnSession(std::string protocol) : protocol_(std::move(protocol))
{
}

Result<SsmTransportConfig> RelearnSession::transport_setup() const
{
    if (protocol_ != "sub_tcu_denso_sh7055_can" && protocol_ != "sub_tcu_denso_sh7058_can")
    {
        return fail(ErrorKind::Unsupported, "not a Subaru Denso SH705x TCU protocol: " + protocol_);
    }
    // legacy :70 -- configureIso15765Can(serial, "500000", 0x7E1, 0x7E9).
    return SsmTransportConfig{};
}

void RelearnSession::submit(GateResponse response)
{
    gate_outstanding_ = false;
    declined_ = response == GateResponse::Decline;
}

ServiceFunctionStep RelearnSession::resume(ISsmTransport& transport, IClock&, const ICancellationToken& cancellation,
                                           IEventSink& events)
{
    if (gate_outstanding_)
    {
        return FailedStep{Error{ErrorKind::Internal, "resume() called with an operator gate outstanding"}};
    }
    if (declined_)
    {
        return FailedStep{Error{ErrorKind::Cancelled, "operator declined a relearn gate"}};
    }
    if (cancellation.cancelled())
    {
        return FailedStep{Error{ErrorKind::Cancelled, "cancelled during TCU relearn"}};
    }

    switch (stage_)
    {
    case Stage::AwaitStaticSetupGate:
        // legacy :648 -- engine at temperature, car off the ground, engine off,
        // ignition on, stick in P.
        stage_ = Stage::WriteSteps;
        gate_outstanding_ = true;
        return GateStep{OperatorGateId::RelearnStaticSetup};

    case Stage::WriteSteps:
    {
        events.log(LogLevel::Info, "Initialising TCU relearn, step 1...");
        const auto first = exchangeTolerantly(transport, cancellation, kStepOne, kWriteAck);
        if (!first.has_value())
        {
            return FailedStep{first.error()};
        }
        if (first->size() <= 4 || (*first)[4] != kWriteAck)
        {
            // legacy :688/:694 -- logged, not fatal.
            events.log(LogLevel::Error, "Wrong response from TCU on relearn step 1; continuing");
        }

        events.log(LogLevel::Info, "Initialising TCU relearn, step 2...");
        const auto second = exchangeTolerantly(transport, cancellation, kStepTwo, kWriteAck);
        if (!second.has_value())
        {
            return FailedStep{second.error()};
        }
        if (second->size() <= 4 || (*second)[4] != kWriteAck)
        {
            // legacy :722/:733 -- logged, not fatal.
            events.log(LogLevel::Error, "Wrong response from TCU on relearn step 2; continuing");
        }

        // legacy :735 -- the gate that cannot be pre-collected.
        stage_ = Stage::Poll;
        gate_outstanding_ = true;
        return GateStep{OperatorGateId::RelearnEngineRunning};
    }

    case Stage::Poll:
    {
        events.log(LogLevel::Info, "Tracking relearn status...");
        RelearnOutcome outcome;
        for (int poll = 0; poll < kPollIterations; ++poll)
        {
            if (cancellation.cancelled())
            {
                return FailedStep{Error{ErrorKind::Cancelled, "cancelled while tracking relearn status"}};
            }

            if (const auto sent = transport.write(kPoll); !sent.has_value())
            {
                return FailedStep{sent.error()};
            }

            const auto received = transport.read(kReadTimeoutMs, cancellation);
            if (!received.has_value())
            {
                return FailedStep{received.error()};
            }

            ++outcome.polls_performed;
            events.progress(outcome.polls_performed, kPollIterations);
            if (received->has_value())
            {
                outcome.last_status_frame = **received;
                if (outcome.last_status_frame.size() > 4 && outcome.last_status_frame[4] != kReadAck)
                {
                    // legacy :771/:777 -- logged, not fatal.
                    events.log(LogLevel::Error, "Unexpected relearn status response; continuing");
                }
            }
        }

        // Which status value means "relearn complete" is not recoverable from
        // the legacy source, so no terminal condition is invented: the bound
        // is the legacy's 200 and the frame is surfaced for the bench.
        stage_ = Stage::Done;
        return CompletedStep{outcome};
    }

    case Stage::AwaitEngineRunningGate:
    case Stage::Done:
        break;
    }

    return FailedStep{Error{ErrorKind::Internal, "relearn resumed after completion"}};
}

} // namespace fastecu::service_functions
```

- [ ] **Step 5: Add the Bazel targets and register the closure**

```python
cc_library(
    name = "relearn_session",
    srcs = ["relearn_session.cpp"],
    hdrs = ["relearn_session.h"],
    deps = [":service_function_session"],
)

fastecu_portable_gtest(
    name = "relearn_session_test",
    srcs = ["relearn_session_test.cpp"],
    deps = [
        ":relearn_session",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/protocol/testing:scripted_transports",
    ],
)
```

Add `"relearn_session"` to `PORTABLE_ROOTS` and the matching label to the genquery.

- [ ] **Step 6: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/service_functions:relearn_session_test`
Expected: PASS, 12 tests.

- [ ] **Step 7: Remove the now-dead `AwaitEngineRunningGate` enumerator**

The state machine returns `GateStep{RelearnEngineRunning}` from `WriteSteps`
and moves straight to `Poll`, so `Stage::AwaitEngineRunningGate` is never
entered. Delete the enumerator from the header and its empty case from the
switch, then re-run the test to confirm it still passes. (Left in the draft
above deliberately: notice it and remove it rather than shipping a state that
cannot be reached — the wave-3 lesson about unreachable code.)

- [ ] **Step 8: Run the full gate and commit**

```sh
bazel test -k --config=release //src/backend/service_functions/... //:portable_closure //:legacy_flash_drain
prek run --all-files
git add src/backend/service_functions BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat(service-functions): port TCU relearn with portable operator gates"
```

**PR 1 ends here.** Open it with the title `feat(service-functions): add the portable TCU service-functions package` and a body that lists the six corrected defects with their legacy line numbers, states that `//:legacy_flash_drain` is deliberately unchanged, and links the design doc.

---

### Task 6: Desktop worker

**Files:**
- Create: `src/platform/desktop/common/service_functions/serial_facade_configurator.h`
- Create: `src/platform/desktop/common/service_functions/serial_facade_configurator.cpp`
- Create: `src/platform/desktop/common/service_functions/service_function_worker.h`
- Create: `src/platform/desktop/common/service_functions/service_function_worker.cpp`
- Create: `src/platform/desktop/common/service_functions/service_function_worker_test.cpp`
- Create: `src/platform/desktop/common/service_functions/BUILD.bazel`

**Interfaces:**
- Consumes: `ServiceFunctionSession`, `SsmTransportConfig`, `ServiceFunctionStep` (Tasks 1-5).
- Produces: `ISerialFacadeConfigurator` (with `Status apply(const SsmTransportConfig&)`), `SerialPortActionsConfigurator`, `ServiceFunctionWorkerResult`, and `ServiceFunctionWorker` — a `QThread` with `requestStop()`, `answerGate(bool)`, and signals `logEvent(int, QString)`, `progressChanged(int, int)`, `gateRequested(int)`, `finished(ServiceFunctionWorkerResult)`.

`ServiceFunctionWorker` mirrors `FlashWorker` (`src/platform/desktop/common/flash/flash_worker.h`) deliberately: same `QThread` subclassing, same `ManualCancellationToken` member, same "emit `finished` exactly once from the worker's own thread" contract, same `requestStop()` semantics. It adds one thing FlashWorker does not need — a gate wait.

Configuration is split out behind `ISerialFacadeConfigurator` so the worker's contract ("`transport_setup()` fails ⇒ the serial facade is never touched") is assertable without a real `SerialPortActions`.

- [ ] **Step 1: Read the reference implementation**

Run:
```sh
sed -n '1,90p' src/platform/desktop/common/flash/flash_worker.h
sed -n '1,60p' src/platform/desktop/common/flash/flash_worker.cpp
sed -n '1,30p' src/platform/desktop/common/flash/flash_worker_test.cpp
```

Note the test suite's opening comment: it waits on condition variables and thread joins and **never** on `QSignalSpy::wait()`, because a real clock's sleeps would race the assertions. The suite below follows that, using a `FakeClock` for the same reason.

- [ ] **Step 2: Write the failing test**

Create `src/platform/desktop/common/service_functions/service_function_worker_test.cpp`:

```cpp
// Teardown, gate, and configuration-ordering coverage for
// ServiceFunctionWorker. Follows flash_worker_test.cpp: a FakeClock plus
// condition variables and thread joins, never QSignalSpy::wait(), so no
// assertion depends on wall-clock timing.
#include "src/platform/desktop/common/service_functions/service_function_worker.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <memory>
#include <vector>

#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::service_functions::GateResponse;
using fastecu::service_functions::GateStep;
using fastecu::service_functions::CompletedStep;
using fastecu::service_functions::FailedStep;
using fastecu::service_functions::ISerialFacadeConfigurator;
using fastecu::service_functions::OperatorGateId;
using fastecu::service_functions::ServiceFunctionSession;
using fastecu::service_functions::ServiceFunctionStep;
using fastecu::service_functions::ServiceFunctionWorker;
using fastecu::service_functions::ServiceFunctionWorkerResult;
using fastecu::service_functions::SetParametersOutcome;
using fastecu::service_functions::SsmTransportConfig;

namespace
{

class RecordingConfigurator final : public ISerialFacadeConfigurator
{
  public:
    fastecu::Status apply(const SsmTransportConfig& config) override
    {
        applied.push_back(config);
        return {};
    }

    std::vector<SsmTransportConfig> applied;
};

// A session whose steps are supplied by the test, so worker behaviour is
// isolated from any real protocol.
class ScriptedSession final : public ServiceFunctionSession
{
  public:
    explicit ScriptedSession(std::vector<ServiceFunctionStep> steps, bool setup_fails = false)
        : steps_(std::move(steps)), setup_fails_(setup_fails)
    {
    }

    fastecu::Result<SsmTransportConfig> transport_setup() const override
    {
        if (setup_fails_)
        {
            return fastecu::fail(ErrorKind::Unsupported, "scripted setup failure");
        }
        return SsmTransportConfig{};
    }

    ServiceFunctionStep resume(ISsmTransport&, fastecu::IClock&, const fastecu::ICancellationToken& cancellation,
                               fastecu::IEventSink&) override
    {
        ++resume_calls;
        if (cancellation.cancelled())
        {
            return FailedStep{fastecu::Error{ErrorKind::Cancelled, "cancelled"}};
        }
        if (next_ >= steps_.size())
        {
            return FailedStep{fastecu::Error{ErrorKind::Internal, "script exhausted"}};
        }
        return steps_[next_++];
    }

    void submit(GateResponse response) override
    {
        submitted.push_back(response);
    }

    int resume_calls = 0;
    std::vector<GateResponse> submitted;

  private:
    std::vector<ServiceFunctionStep> steps_;
    bool setup_fails_;
    std::size_t next_ = 0;
};

struct Harness
{
    RecordingConfigurator configurator;
    ScriptedSession *session = nullptr;
    std::unique_ptr<ServiceFunctionWorker> worker;

    void build(std::vector<ServiceFunctionStep> steps, bool setup_fails = false)
    {
        auto owned = std::make_unique<ScriptedSession>(std::move(steps), setup_fails);
        session = owned.get();
        worker = std::make_unique<ServiceFunctionWorker>(std::move(owned), std::make_unique<ScriptedSsmTransport>(),
                                                         std::make_unique<FakeClock>(), &configurator);
    }
};

} // namespace

class ServiceFunctionWorkerTest : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        qRegisterMetaType<ServiceFunctionWorkerResult>("fastecu::service_functions::ServiceFunctionWorkerResult");
    }

    void completesWithoutEverRequestingAGate()
    {
        Harness harness;
        harness.build({CompletedStep{SetParametersOutcome{.frames_written = 12}}});
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(gates.count(), 0);
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(result.success);
    }

    void appliesTheSessionsTransportConfigurationBeforeRunning()
    {
        Harness harness;
        harness.build({CompletedStep{SetParametersOutcome{}}});

        harness.worker->start();
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(harness.configurator.applied.size(), std::size_t{1});
        QCOMPARE(harness.configurator.applied.front(), SsmTransportConfig{});
    }

    void neverTouchesTheSerialFacadeWhenSetupFails()
    {
        Harness harness;
        harness.build({}, /*setup_fails=*/true);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QVERIFY(harness.worker->wait(5000));

        QVERIFY(harness.configurator.applied.empty());
        QCOMPARE(harness.session->resume_calls, 0);
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Unsupported);
    }

    void blocksOnAGateUntilItIsAnswered()
    {
        Harness harness;
        harness.build({GateStep{OperatorGateId::RelearnEngineRunning}, CompletedStep{SetParametersOutcome{}}});
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QTRY_COMPARE(gates.count(), 1);
        QCOMPARE(gates.at(0).at(0).toInt(), static_cast<int>(OperatorGateId::RelearnEngineRunning));
        QCOMPARE(done.count(), 0);          // still parked on the gate
        QCOMPARE(harness.session->resume_calls, 1);

        harness.worker->answerGate(true);
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(harness.session->submitted, std::vector<GateResponse>{GateResponse::Accept});
        QCOMPARE(done.count(), 1);
    }

    void aDeclinedGateReachesTheSessionAsDecline()
    {
        Harness harness;
        harness.build({GateStep{OperatorGateId::RelearnStaticSetup},
                       FailedStep{fastecu::Error{ErrorKind::Cancelled, "operator declined a relearn gate"}}});
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QTRY_COMPARE(gates.count(), 1);
        harness.worker->answerGate(false);
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(harness.session->submitted, std::vector<GateResponse>{GateResponse::Decline});
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Cancelled);
    }

    void requestStopUnblocksAnOutstandingGate()
    {
        // The teardown contract this class exists for: a worker parked on a
        // gate must not hold the thread open when the dialog closes.
        Harness harness;
        harness.build({GateStep{OperatorGateId::RelearnEngineRunning}, CompletedStep{SetParametersOutcome{}}});
        QSignalSpy gates(harness.worker.get(), &ServiceFunctionWorker::gateRequested);
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QTRY_COMPARE(gates.count(), 1);
        harness.worker->requestStop();
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::Cancelled);
    }

    void emitsFinishedExactlyOnceOnFailure()
    {
        Harness harness;
        harness.build({FailedStep{fastecu::Error{ErrorKind::BadResponse, "TCU said no"}}});
        QSignalSpy done(harness.worker.get(), &ServiceFunctionWorker::finished);

        harness.worker->start();
        QVERIFY(harness.worker->wait(5000));

        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<ServiceFunctionWorkerResult>();
        QCOMPARE(result.error_kind, ErrorKind::BadResponse);
        QCOMPARE(result.error_detail, QString("TCU said no"));
    }
};

QTEST_MAIN(ServiceFunctionWorkerTest)
#include "service_function_worker_test.moc"
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `bazel test --config=release //src/platform/desktop/common/service_functions:test_service_function_worker`
Expected: FAIL — the package does not exist.

- [ ] **Step 4: Write the configurator**

Create `src/platform/desktop/common/service_functions/serial_facade_configurator.h`:

```cpp
#pragma once

#include "src/backend/ports/result.h"
#include "src/backend/service_functions/service_function_types.h"

class SerialPortActions;

namespace fastecu::service_functions
{

// Applies a session's SsmTransportConfig to a serial facade. Behind an
// interface so the worker's "setup failure touches no hardware" contract is
// assertable without a real SerialPortActions.
class ISerialFacadeConfigurator
{
  public:
    virtual ~ISerialFacadeConfigurator() = default;
    virtual Status apply(const SsmTransportConfig& config) = 0;
};

class SerialPortActionsConfigurator final : public ISerialFacadeConfigurator
{
  public:
    explicit SerialPortActionsConfigurator(SerialPortActions *serial) : serial_(serial)
    {
    }

    Status apply(const SsmTransportConfig& config) override;

  private:
    SerialPortActions *serial_;
};

} // namespace fastecu::service_functions
```

And `serial_facade_configurator.cpp`:

```cpp
#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

#include <QString>

#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::service_functions
{

Status SerialPortActionsConfigurator::apply(const SsmTransportConfig& config)
{
    if (serial_ == nullptr)
    {
        return fail(ErrorKind::Disconnected, "no serial facade");
    }

    serial_->reset_connection();
    if (config.framing == SsmTransportConfig::Framing::Iso15765)
    {
        // legacy :70 -- FlashUtils::configureIso15765Can(serial, "500000", 0x7E1, 0x7E9).
        serial_->set_is_iso14230_connection(false);
        serial_->set_is_can_connection(false);
        serial_->set_is_iso15765_connection(true);
        serial_->set_is_29_bit_id(false);
        serial_->set_can_speed(QString::number(config.bitrate_or_baud));
        serial_->set_iso15765_source_address(config.request_id);
        serial_->set_iso15765_destination_address(config.response_id);
        serial_->set_can_source_address(config.request_id);
        serial_->set_can_destination_address(config.response_id);
        serial_->open_serial_port();
    }
    else
    {
        // legacy :141-152 -- "CAN 0xb8 command is disabled, so switch to
        // K-Line comms". Note the legacy order: open first, then set the
        // speed, then turn the driver's auto-header off.
        serial_->set_is_can_connection(false);
        serial_->set_is_iso15765_connection(false);
        serial_->set_is_iso14230_connection(true);
        serial_->open_serial_port();
        serial_->change_port_speed(QString::number(config.bitrate_or_baud));
        serial_->set_add_iso14230_header(config.add_iso14230_header);
    }

    if (!serial_->is_serial_port_open())
    {
        return fail(ErrorKind::Disconnected, "serial port did not open");
    }
    return {};
}

} // namespace fastecu::service_functions
```

- [ ] **Step 5: Write the worker header**

Create `src/platform/desktop/common/service_functions/service_function_worker.h`:

```cpp
#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <memory>
#include <optional>

#include "src/backend/ports/clock.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/protocol/issm_transport.h"
#include "src/backend/service_functions/service_function_session.h"
#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"

namespace fastecu::service_functions
{

// Qt-friendly mirror of a terminal ServiceFunctionStep, for the same reason
// FlashWorkerResult exists: std::expected and std::variant are not Qt
// metatypes, and QSignalSpy / QueuedConnection copy through QVariant.
struct ServiceFunctionWorkerResult
{
    bool success = false;
    ErrorKind error_kind = ErrorKind::Internal;
    QString error_detail;
    std::optional<ServiceFunctionOutcome> outcome;
};

// Qt lifecycle adapter for a portable ServiceFunctionSession. Mirrors
// FlashWorker exactly except for the gate wait, which FlashWorker has no need
// for: flash collects every confirmation before execution, and relearn cannot.
class ServiceFunctionWorker final : public QThread
{
    Q_OBJECT

  public:
    ServiceFunctionWorker(std::unique_ptr<ServiceFunctionSession> session, std::unique_ptr<ISsmTransport> transport,
                          std::unique_ptr<IClock> clock, ISerialFacadeConfigurator *configurator,
                          QObject *parent = nullptr);
    ~ServiceFunctionWorker() override;

    ServiceFunctionWorker(const ServiceFunctionWorker&) = delete;
    ServiceFunctionWorker& operator=(const ServiceFunctionWorker&) = delete;

    // Cancels the token and wakes any outstanding gate wait. Safe from any
    // thread, any number of times, before or after start().
    void requestStop();

    // Answers the outstanding gate. Safe from any thread.
    void answerGate(bool accepted);

  signals:
    void logEvent(int level, QString message);
    void progressChanged(int done, int total);
    void gateRequested(int gateId);
    // Emitted exactly once per run(), always from this worker's own thread.
    void finished(fastecu::service_functions::ServiceFunctionWorkerResult result);

  protected:
    void run() override;

  private:
    // Blocks until answerGate() or requestStop(). Returns nullopt on stop.
    std::optional<GateResponse> waitForGate();

    std::unique_ptr<ServiceFunctionSession> session_;
    std::unique_ptr<ISsmTransport> transport_;
    std::unique_ptr<IClock> clock_;
    ISerialFacadeConfigurator *configurator_;
    ManualCancellationToken cancellation_;

    QMutex gate_mutex_;
    QWaitCondition gate_answered_;
    std::optional<GateResponse> gate_response_;
    bool stopping_ = false;
};

} // namespace fastecu::service_functions

Q_DECLARE_METATYPE(fastecu::service_functions::ServiceFunctionWorkerResult)
```

- [ ] **Step 6: Write the worker implementation**

`run()` is: `transport_setup()` → on failure emit `finished` and return without touching the configurator → `configurator_->apply()` → then loop `session_->resume(...)`, dispatching each step. A `GateStep` emits `gateRequested` and calls `waitForGate()`; a `nullopt` return (stop requested) finishes with `Cancelled`, otherwise `session_->submit(response)` and loop. `CompletedStep` and `FailedStep` emit `finished` and return.

An `IEventSink` adapter forwards `log` to `logEvent` and `progress` to `progressChanged`, matching how `FlashWorker` bridges the same two.

`requestStop()` sets `cancellation_`, then under `gate_mutex_` sets `stopping_ = true` and calls `gate_answered_.wakeAll()`.

- [ ] **Step 7: Write the BUILD file**

Model on `src/platform/desktop/common/flash/BUILD.bazel`. Use `qt_cc_library` with `service_function_worker.h` in `MOC_HDRS` (it has `Q_OBJECT`; a header missing from `MOC_HDRS` links but fails at runtime) and `serial_facade_configurator.h` in `normal_hdrs`. The test is a `fastecu_qttest`, which takes `src` (singular), not `srcs`:

```python
fastecu_qttest(
    name = "test_service_function_worker",
    src = "service_function_worker_test.cpp",
    deps = [
        ":service_function_worker",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/protocol/testing:scripted_transports",
    ],
)
```

Do **not** add this package to `PORTABLE_ROOTS` or the genquery — it is Qt by design.

- [ ] **Step 8: Run the test to verify it passes**

Run: `bazel test --config=release //src/platform/desktop/common/service_functions:test_service_function_worker`
Expected: PASS, 7 tests.

- [ ] **Step 9: Run the full gate and commit**

```sh
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:portable_closure //:serial_compat_allowlist //:legacy_flash_drain
prek run --all-files
git add src/platform/desktop/common/service_functions
git commit -m "feat(service-functions): add the desktop worker driving portable sessions"
```

---

### Task 7: ServiceFunctionDialog

**Files:**
- Create: `src/ui/desktop/service_functions/service_function_dialog.h`
- Create: `src/ui/desktop/service_functions/service_function_dialog.cpp`
- Create: `src/ui/desktop/service_functions/service_function_dialog_test.cpp`
- Create: `src/ui/desktop/service_functions/BUILD.bazel`

**Interfaces:**
- Consumes: `ServiceFunctionWorker` (Task 6), the three sessions (Tasks 3-5).
- Produces: `ServiceFunctionKind` (`Relearn`, `ReadParameters`, `SetParameters`) and `ServiceFunctionDialog`, constructed as `(SerialPortActions*, std::string protocol, ServiceFunctionKind, QWidget* parent)`, with public `TcuParameterValues collectedValues() const`, `void showReadout(const TcuParameterReadout&)`, and `bool askGate(OperatorGateId)`.

The three public accessors exist so the form↔struct mapping and the readout
rendering are testable without a worker, a thread, or a serial port. Widgets
are built in code (no `.ui` file) so the spin boxes can carry stable
`objectName`s the test looks up.

- [ ] **Step 1: Write the failing test**

Create `src/ui/desktop/service_functions/service_function_dialog_test.cpp`:

```cpp
#include "src/ui/desktop/service_functions/service_function_dialog.h"

#include <QSpinBox>
#include <QTableWidget>
#include <QTest>

using fastecu::service_functions::ServiceFunctionDialog;
using fastecu::service_functions::ServiceFunctionKind;
using fastecu::service_functions::TcuParameterReadout;

namespace
{

QSpinBox *box(ServiceFunctionDialog& dialog, const char *name)
{
    return dialog.findChild<QSpinBox *>(QString::fromLatin1(name));
}

} // namespace

class ServiceFunctionDialogTest : public QObject
{
    Q_OBJECT

  private slots:
    void setParametersSpinBoxesCarryTheLegacyPromptBounds()
    {
        // legacy :162-202 -- eight prompts bounded 0-255 and one bounded
        // 0-65535. The value model makes these unrepresentable rather than
        // rejectable, so this is where the bounds are actually asserted.
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::SetParameters};

        for (const char *name : {"correction_1to2", "correction_2to3", "correction_3to4", "correction_4to5",
                                 "correction_forward_brake", "correction_four_wheel_drive",
                                 "correction_line_pressure", "temperature_basis"})
        {
            QSpinBox *spin = box(dialog, name);
            QVERIFY2(spin != nullptr, name);
            QCOMPARE(spin->minimum(), 0);
            QCOMPARE(spin->maximum(), 255);
        }

        QSpinBox *torque = box(dialog, "torque_correction_awd");
        QVERIFY(torque != nullptr);
        QCOMPARE(torque->minimum(), 0);
        QCOMPARE(torque->maximum(), 65535);
    }

    void everyFormFieldLandsInItsOwnStructMember()
    {
        // Guards against a form-to-struct mix-up, which the wire-order table
        // in tcu_parameter_table_test cannot catch: nine distinct values in,
        // nine distinct members out.
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::SetParameters};

        box(dialog, "correction_1to2")->setValue(0x11);
        box(dialog, "correction_2to3")->setValue(0x22);
        box(dialog, "correction_3to4")->setValue(0x33);
        box(dialog, "correction_4to5")->setValue(0x44);
        box(dialog, "correction_forward_brake")->setValue(0x55);
        box(dialog, "correction_four_wheel_drive")->setValue(0x66);
        box(dialog, "correction_line_pressure")->setValue(0x77);
        box(dialog, "temperature_basis")->setValue(0x88);
        box(dialog, "torque_correction_awd")->setValue(0xbeef);

        const auto values = dialog.collectedValues();
        QCOMPARE(values.correction_1to2, 0x11);
        QCOMPARE(values.correction_2to3, 0x22);
        QCOMPARE(values.correction_3to4, 0x33);
        QCOMPARE(values.correction_4to5, 0x44);
        QCOMPARE(values.correction_forward_brake, 0x55);
        QCOMPARE(values.correction_four_wheel_drive, 0x66);
        QCOMPARE(values.correction_line_pressure, 0x77);
        QCOMPARE(values.temperature_basis, 0x88);
        QCOMPARE(values.torque_correction_awd, 0xbeef);
    }

    void setParametersFormIsOneDialogNotNineModals()
    {
        // The legacy asks nine sequential QInputDialogs (:162-202); this shows
        // all nine at once so the operator can review before any write.
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::SetParameters};
        QCOMPARE(dialog.findChildren<QSpinBox *>().count(), 9);
    }

    void readParametersRendersAllNineLabelledValues()
    {
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::ReadParameters};
        dialog.showReadout(TcuParameterReadout{
            .input_clutch = 0x11,
            .high_low_reverse_clutch = 0x22,
            .direct_clutch = 0x33,
            .front_brake = 0x44,
            .awd_clutch_torque = 0xbeef,
            .forward_brake = 0x55,
            .four_wheel_drive = 0x66,
            .line_pressure = 0x77,
            .temperature_basis = 0x88,
        });

        auto *table = dialog.findChild<QTableWidget *>("readout");
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 9);
        QCOMPARE(table->item(0, 0)->text(), QString("Input Clutch Pressure Correction"));
        QCOMPARE(table->item(0, 1)->text(), QString("17"));
        QCOMPARE(table->item(4, 0)->text(), QString("Correction of AWD Clutch Torque"));
        QCOMPARE(table->item(4, 1)->text(), QString("48879"));
        QCOMPARE(table->item(8, 0)->text(), QString("Temperature basis for above Pressure Corrections"));
    }

    void readParametersHasNoSpinBoxes()
    {
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::ReadParameters};
        QCOMPARE(dialog.findChildren<QSpinBox *>().count(), 0);
    }
};

QTEST_MAIN(ServiceFunctionDialogTest)
#include "service_function_dialog_test.moc"
```

The readout row labels are the legacy's own strings (`:611-624`), preserved
verbatim so an operator sees the same words. Row 4's `48879` is `0xbeef`
decimal, matching the legacy's `QString::number` of the raw word.

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/ui/desktop/service_functions:test_service_function_dialog`
Expected: FAIL — the package does not exist.

- [ ] **Step 3: Implement the dialog**

Build widgets in code. For `SetParameters`, nine `QSpinBox`es in a `QFormLayout`, each with the `objectName` the test looks up and the label text taken verbatim from the legacy prompt (`:162-202`, e.g. "1->2 Pressure Correction (DC):"). For `ReadParameters`, a `QTableWidget` named `readout` with two columns. All three kinds get a progress bar wired to `progressChanged` and a log pane wired to `logEvent`.

`askGate(OperatorGateId)` shows a `QMessageBox` whose text lives here, keyed on the gate, both preserved verbatim from the legacy:

```cpp
switch (id)
{
case OperatorGateId::RelearnStaticSetup:
    // legacy :649-651
    text = tr("Engine must be at operating temperature. Car must be off the ground! "
              "Start with Engine off, Ignition on, stick in P, press OK to continue");
    break;
case OperatorGateId::RelearnEngineRunning:
    // legacy :736
    text = tr("Start Engine, let revs settle, move stick into D, fully press brake, press OK to continue");
    break;
}
```

Connect `ServiceFunctionWorker::gateRequested` to a slot that calls `askGate` and then `worker->answerGate(accepted)`. Connect the dialog's `closeEvent` to `worker->requestStop()`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `bazel test --config=release //src/ui/desktop/service_functions:test_service_function_dialog`
Expected: PASS, 5 tests.

- [ ] **Step 5: Run the full gate and commit**

```sh
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:portable_closure //:legacy_flash_drain
prek run --all-files
git add src/ui/desktop/service_functions
git commit -m "feat(service-functions): add the TCU service-function dialog"
```

---

### Task 8: Dispatch the three actions and delete the legacy methods

**Files:**
- Modify: `src/ui/desktop/flash/tcu/flash_tcu_subaru_denso_sh705x_can.cpp:32-100`
- Modify: `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_denso_sh705x_can_operation.{h,cpp}`
- Modify: `src/platform/desktop/common/flash/legacy/tcu/BUILD.bazel` if `promptInt`'s removal drops a dep

**Interfaces:**
- Consumes: `ServiceFunctionDialog` (Task 7).
- Produces: nothing new.

- [ ] **Step 1: Rewrite the action dispatch**

In `run()`, keep the four-button picker exactly as it is. Change the branches: "Dump" keeps `tcuAction = 1` and the existing legacy path untouched; "Relearn", "Read Param" and "Set Param" each construct a `ServiceFunctionDialog` with the matching `ServiceFunctionKind`, show it, and return without constructing `FlashTcuSubaruDensoSH705xCanOperation`.

- [ ] **Step 2: Delete the three legacy methods and `promptInt`**

Remove from the `.cpp`: `tcu_setparam_subaru_ssm` (`:135-517`), `tcu_readparam_subaru_ssm` (`:517-632`), `tcu_relearn_subaru_ssm` (`:632-790`), and `promptInt` (`:113-135`). Remove their declarations and the now-unused `tcuAction` member from the `.h`, and drop the `tcuAction` constructor parameter. Simplify `execute()` to the `tcuAction == 1` body.

- [ ] **Step 3: Verify the drain ratchet is unchanged**

Run: `bazel test --config=release //:legacy_flash_drain`
Expected: PASS with the same 14 entries. The file still includes `serial_port_actions.h` for the flash path, so its entry must remain. If the ratchet reports the entry as removed, the deletion went too far — the flash path must stay.

- [ ] **Step 4: Run the full gate**

```sh
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
prek run --all-files
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop/flash/tcu src/platform/desktop/common/flash/legacy/tcu
git commit -m "refactor(service-functions): dispatch TCU actions 2-4 to the portable sessions"
```

---

### Task 9: The ledger

**Files:**
- Modify: `docs/flash-qualification-matrix.md`
- Modify: `docs/modularization-plan.md`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing.

- [ ] **Step 1: Add the Service functions section to the matrix**

Add a clearly separated section below the flash table, with three rows. Each row carries `portable=yes`, `hardware_status=experimental`, the new test labels in `automated_evidence`, and `notes` naming the corrections:

- **TcuRelearn** — notes: unconditional `return STATUS_ERROR` at legacy `:786` removed so completion reports success; poll accepts `0xE8` rather than the unreachable `0xF8`; poll frame composed directly because legacy `:740-747` writes three bytes past the end of a 9-byte buffer. **VERIFY: the poll's terminal condition is unresolved — the 200-iteration bound is preserved and the last status frame is surfaced; a bench must establish which status value means complete.**
- **TcuReadParameters** — notes: retry loop accepts `0xE8`, so the operation can succeed for the first time (legacy `:571-608` demanded `0xF8` then `0xE8`); response must be ≥15 bytes because the decode indexes byte 14 while legacy guarded `> 10`; results returned as data rather than log lines.
- **TcuSetParameters** — notes: all twelve frames composed from a table and framed once each. Legacy `:215` reassigned `output` to the framed array and `:237` onward mutated the SSM length byte and service ID, so only write 1 was well-formed and the operation aborted on write 2 with one correction applied and uncommitted. **This is the largest behavior change in the work: the operation begins doing what its UI has always claimed. First bench item for this family.**

- [ ] **Step 2: Update the modularization plan**

In step 5's bullet list, extend "Split `FileActions` and `MainWindow` responsibilities into definition, calibration, checksum, logging, and flash use cases" to name service functions as a sixth, and add a line to the Status section recording the new package with its PR numbers and the note that it is groundwork for wave 5.

- [ ] **Step 3: Verify the links resolve**

Run: `prek run --all-files`
Expected: lychee passes. Cross-document references must be links with human-readable text, not backticked paths — lychee cannot see a path written as inline code.

- [ ] **Step 4: Commit**

```bash
git add docs/flash-qualification-matrix.md docs/modularization-plan.md
git commit -m "docs: record the TCU service functions in the matrix and the plan"
```

**PR 2 ends here.** Open it with the title `feat(service-functions): wire the TCU service functions into the desktop` and a body that states the drain ratchet is deliberately unchanged and links PR 1.
