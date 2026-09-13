# Step 5 Tail Wave 5 — Denso SH705x CAN Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the four remaining Denso SH705x CAN legacy flash families with portable plans and synchronous executors, preserve the Denso TCU service-function chooser, and reduce `//:legacy_flash_drain` from 14 families to 10.

**Architecture:** The three ISO-15765-only families continue to implement `ICanFlashExecutor` over `ICanFlashTransport`. DensoCAN adds a separate `IMixedCanFlashExecutor`/`IMixedCanFlashTransport` capability that starts in 11-bit ISO-15765, can transition to 29-bit raw CAN for bootloader upload, and returns to ISO-15765 for proprietary `BEEF` kernel exchanges. A kernel-backed desktop workflow resolves kernel bytes before I/O and binds either transport shape to the existing `FlashWorker`/`FlashDialog`; exact routing prevents unqualified protocol suffixes from being claimed.

**Tech Stack:** C++23, Bazel, GoogleTest/GoogleMock, Qt 6 desktop adapters and dialogs, `SerialPortActions`, existing portable flash/UDS/SSM/checksum libraries.

**Spec:** [docs/superpowers/specs/2026-09-02-step5-tail-wave5-denso-sh705x-can-design.md](../specs/2026-09-02-step5-tail-wave5-denso-sh705x-can-design.md)

## Global Constraints

- Base implementation work on `origin/master` commit `59f4e442` plus the approved design commit `95e8019c` and its 2026-09-07 refresh. Create an isolated worktree with `superpowers:using-git-worktrees` before executing the plan.
- Keep executors synchronous, bounded, dialog-free, filesystem-free, and independent of `EcuCalDefStructure` and Qt.
- Preserve caller-owned transport lifetime from ADR 0015: `BoundFlashAttempt` calls initial `configure`, initial `open`, and final `close`; executors never call those outer-lifecycle methods. DensoCAN's two mode-transition methods encapsulate protocol-required reset/configure/reopen operations.
- Do not add raw operations to `ICanFlashTransport`. A compile-time type mismatch must prevent an ISO-only executor from receiving a mixed transport and vice versa.
- Treat ISO-15765 as transport, not application semantics. Use `CanFlashUdsChannel`/`uds::UdsClient` only where legacy obeys positive-response/NRC rules. Keep tolerant vendor queries and proprietary `BEEF` kernel messages as explicit channel or transport exchanges.
- Every transcribed exchange must cite the legacy file and line range at revision `59f4e442`. The four Wave 5 legacy operation sources are byte-for-byte unchanged from the original `cd9ab679` evidence baseline. Executor tests independently spell expected bytes rather than importing the production constants they verify.
- Preserve unexplained per-family differences in timing, retry counts, response tolerance, flash geometry, addresses, padding, and ordering. Factor only after all four ports pass their independent tests.
- Do not widen or reuse `src/backend/flash/ecu/single_window_plan.{h,cpp}` for Wave 5. It is intentionally kernel-free, rejects test-write, and models one write/erase window; Wave 5 carries kernels, three families support test-write, and its Denso families preserve 16-block geometry.
- Reuse the current `FlashAttemptOutcome` helper in new desktop workflows; do not duplicate terminal, failure, cancellation, read-byte, or ROM-ID outcome state.
- A behavior correction is allowed only with all four artifacts in the same family commit: local proof of a defect, a failing reproduction/invariant test, an exact corrected-behavior test, and a qualification-matrix note. Sibling similarity alone does not authorize a correction.
- Automated completion changes each migrated row to `portable=yes`, `hardware_status=experimental`; do not claim bench or vehicle qualification.
- New Wave 5 routes use exact equality. Existing prefix routes retain their current behavior and priority, including EEPROM routes ahead of DensoCAN.
- Preserve the existing error taxonomy: `InvalidConfig` for plans/catalog/geometry, `Timeout` for exhausted bounded reads, `Disconnected` for configure/open/adapter loss, `BadResponse` for malformed or negative replies, `Cancelled` for cooperative cancellation, `Unsupported` for rejected operations, and `Internal` only for invariant violations or unexpected adapter exceptions.

### Exact protocol and capability matrix

| Family | Exact protocol | MCU | Operations |
|---|---|---|---|
| DensoCAN | `sub_ecu_denso_sh7055_densocan` | `SH7055` | read, test-write, write |
| DensoCAN | `sub_ecu_denso_sh7058_densocan` | `SH7058` | read, test-write, write |
| DensoCAN | `sub_ecu_denso_sh7058s_densocan` | `SH7058` | read, test-write, write |
| DensoCAN | `sub_ecu_denso_sh7058s_diesel_densocan` | `SH7058` | read, test-write, write |
| DensoCAN | `sub_ecu_denso_sh7059_diesel_densocan` | `SH7059d` | read, test-write, write |
| TCU | `sub_tcu_denso_sh7055_can` | `SH7055` | read only |
| TCU | `sub_tcu_denso_sh7058_can` | `SH7058` | read, write |
| Petrol ECU | `sub_ecu_denso_sh7058_can` | `SH7058` | read, test-write, write |
| Petrol ECU | `sub_ecu_denso_sh7058_can_ecutek` | `SH7058` | read, test-write, write |
| Petrol ECU | `sub_ecu_denso_sh7058_can_ecutek_racerom` | `SH7058` | read, test-write, write |
| Petrol ECU | `sub_ecu_denso_sh7058_can_ecutek_racerom_alt` | `SH7058` | read, test-write, write |
| Petrol ECU | `sub_ecu_denso_sh7058_can_cobb` | `SH7058` | read, test-write, write |
| Diesel ECU | `sub_ecu_denso_sh7058_can_diesel` | `SH7058d` | read, test-write, write |
| Diesel ECU | `sub_ecu_denso_sh7059_can_diesel` | `SH7059d` | read, test-write, write |

Near misses such as `sub_ecu_denso_sh7058_can_future`, `sub_tcu_denso_sh7058_can_typo`, and `sub_ecu_denso_sh7058_densocan_extra` must remain unrouted.

### Shared wire invariants

- ISO-only ECU configuration: `bitrate=500000`, `request_id=0x7E0`, `response_id=0x7E8`, `extended_id=false`. The TCU deliberately differs: `request_id=0x7E1`, `response_id=0x7E9`, `extended_id=false` (`flash_tcu_subaru_denso_sh705x_can_operation.cpp:55-58`).
- DensoCAN mixed configuration: kernel ISO values above; raw `bitrate=500000`, `transmit_id=0x000FFFFE`, `receive_id=0x21`, `extended_id=true`.
- DensoCAN order when upload is required: ISO kernel probe → raw transition → exactly 1,000 wake frames with a cancellation check before each frame → receive-buffer clear → bootloader handshake/upload → ISO transition → kernel-ID verification → ROM operation.
- DensoCAN raw frames carry one explicit arbitration ID plus an eight-byte payload. The desktop adapter alone converts that value to/from `SerialPortActions`' four-byte big-endian ID prefix.
- Kernel uploads and ROM transfers preserve the legacy chunk sizes: DensoCAN bootloader upload uses six-byte blocks; ISO-only kernel upload uses 128-byte blocks; kernel ROM reads use `0x400`-byte pages; flash programming uses the legacy-reported message/block sizes with `0x1000` as the established block fallback.
- The common SSM index transformation is `{05,06,07,01,09,0C,0D,08,0A,0D,02,0B,0F,04,00,03,0B,04,06,00,0F,02,0D,09,05,0C,01,0A,03,0D,0E,08}`. DensoCAN payload keys are encrypt `{7856,CE22,F513,6E86}` and decrypt `{6E86,F513,CE22,7856}`. ISO-only payload keys remain `{C85B,32C0,E282,92A0}` and reverse for decrypt.
- Petrol security is a plan enum, not suffix logic inside the executor: `Stock`, `EcuTek`, `RaceRom`, `RaceRomAlt`, `Cobb`. Pin the stock, EcuTek, RaceRom RSA (`d=0x0A863281`, `n=0x0FDA9293`), RaceRom-alt alteration, and Cobb results with fixed seed vectors.
- Geometry comes from `flashdevices[]` and is cross-checked by every builder: `SH7055=0x80000`, `SH7058/SH7058d=0x100000`, `SH7059d=0x180000`, each with the exact 16 blocks in `kernelmemorymodels.h:124-168,209-214`.
- The DensoCAN workflow presents standard `Begin` plus the plan's one up-front `CycleIgnition` confirmation. The other three family plans declare no extra confirmation and rely on standard `Begin`. Cancelling any preflight performs no transport I/O.

## File Structure

```text
src/backend/flash/
  BUILD.bazel                                       (modify per family: type dependencies)
  bound_flash_attempt_test.cpp                     (modify: mixed lifecycle)
  flash_executor.h                                  (modify: mixed contracts/config)
  flash_plan.cpp                                    (modify per family: experimental ID)
  flash_types.h                                     (modify: kind, family enums, variants)
  flash_types_test.cpp                              (modify: transport/family coverage)
  flash_validation.cpp                              (modify per family: family/transport/variant registry)
  flash_validation_test.cpp                         (modify per family: exhaustive registry coverage)
  testing/scripted_mixed_can_flash_transport.h      (create)
  testing/scripted_flash_transports_test.cpp        (modify)
  testing/BUILD.bazel                               (modify)

src/backend/flash/ecu/
  subaru_denso_sh705x_densocan_types.h              (create)
  subaru_denso_sh705x_densocan_plan.{h,cpp}         (create)
  subaru_denso_sh705x_densocan_{plan,executor}_test.cpp (create)
  subaru_denso_sh705x_densocan_executor.{h,cpp}     (create)
  subaru_tcu_denso_sh705x_can_types.h               (create)
  subaru_tcu_denso_sh705x_can_plan.{h,cpp}          (create)
  subaru_tcu_denso_sh705x_can_{plan,executor}_test.cpp (create)
  subaru_tcu_denso_sh705x_can_executor.{h,cpp}      (create)
  subaru_denso_sh7058_can_types.h                   (create)
  subaru_denso_sh7058_can_plan.{h,cpp}              (create)
  subaru_denso_sh7058_can_{plan,executor}_test.cpp  (create)
  subaru_denso_sh7058_can_executor.{h,cpp}          (create)
  subaru_denso_sh7058_can_diesel_types.h            (create)
  subaru_denso_sh7058_can_diesel_plan.{h,cpp}       (create)
  subaru_denso_sh7058_can_diesel_{plan,executor}_test.cpp (create)
  subaru_denso_sh7058_can_diesel_executor.{h,cpp}   (create)
  BUILD.bazel                                       (modify per family)

src/platform/desktop/common/transport/
  desktop_mixed_can_flash_transport.{h,cpp}         (create)
  desktop_mixed_can_flash_transport_test.cpp        (create)
  BUILD.bazel                                       (modify)
src/platform/desktop/common/serial/testing/
  fake_backend.h                                    (modify: clear-buffer seam)
src/platform/desktop/common/flash/
  flash_workflow.cpp                                (modify: kernel workflow/routes)
  flash_workflow_test.cpp                           (modify: 14 routes/near misses)
  BUILD.bazel                                       (modify)
src/ui/desktop/service_functions/
  denso_tcu_read_preflight.{h,cpp}                  (create)
  denso_tcu_read_preflight_test.cpp                 (create)
  BUILD.bazel                                       (modify)
src/ui/desktop/
  mainwindow.{h,cpp}                                (modify: chooser before factory)
  BUILD.bazel                                       (modify)

src/platform/desktop/common/flash/legacy/{ecu,tcu}/ (delete four operation pairs)
src/ui/desktop/flash/{ecu,tcu}/                     (delete four dialog pairs and TCU test)
src/platform/desktop/common/flash/legacy/BUILD.bazel (modify per family)
src/ui/desktop/flash/{ecu,tcu}/BUILD.bazel           (modify per family)
scripts/check-legacy-flash-drain.py                  (modify 14 → 10 incrementally)
BUILD.bazel                                          (modify portable roots)
scripts/check-portable-closure.py                    (modify portable roots)
docs/flash-qualification-matrix.md                   (modify per family)
docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md (modify closeout)
docs/modularization-plan.md                          (modify closeout)
```

---

### Task 1: Mixed-CAN portable contracts and scripted transport

**Files:**
- Modify: `src/backend/flash/bound_flash_attempt_test.cpp`
- Modify: `src/backend/flash/flash_executor.h`
- Modify: `src/backend/flash/flash_types.h`
- Modify: `src/backend/flash/flash_types_test.cpp`
- Modify: `src/backend/flash/flash_validation_test.cpp`
- Create: `src/backend/flash/testing/scripted_mixed_can_flash_transport.h`
- Modify: `src/backend/flash/testing/scripted_flash_transports_test.cpp`
- Modify: `src/backend/flash/testing/BUILD.bazel`

**Interfaces:**
- Consumes: `cdbg::CanFrame { std::uint32_t id; bytes::Bytes payload; }`; existing `IFlashTransport`, `Iso15765Config`, `BoundAttempt`, cancellation/result types.
- Produces: `RawCanConfig`, `MixedCanConfig`, `IMixedCanFlashTransport`, `IMixedCanFlashExecutor`, and `ScriptedMixedCanFlashTransport` with deterministic ISO/raw scripts and transition records.

- [ ] **Step 1: Write compile-time and lifecycle tests for the new capability**

```cpp
static_assert(std::derived_from<TestMixedExecutor::TransportType, IMixedCanFlashTransport>);
static_assert(!std::derived_from<ICanFlashTransport, IMixedCanFlashTransport>);
static_assert(std::variant_size_v<FamilyPlan> ==
              std::tuple_size_v<std::remove_reference_t<decltype(family_cases())>>);

TEST(BoundAttempt, MixedTransportUsesOuterLifecycleExactlyOnce)
{
    auto attempt = bind_flash_attempt(built_plan(), std::make_unique<TestMixedExecutor>(),
                                      std::make_unique<RecordingMixedTransport>());
    ASSERT_TRUE(attempt->run(clock, cancellation, events).has_value());
    EXPECT_THAT(calls, ElementsAre("configure", "open", "execute", "close"));
}
```

Run: `bazel test --config=release //src/backend/flash:bound_flash_attempt_test //src/backend/flash:flash_validation_test`

Expected: FAIL because the mixed contracts do not exist and the current registry test lists only seven of the 15 existing `FamilyPlan` alternatives.

- [ ] **Step 2: Add the narrow mixed contracts**

```cpp
struct RawCanConfig
{
    int bitrate;
    std::uint32_t transmit_id;
    std::uint32_t receive_id;
    bool extended_id;
};

struct MixedCanConfig
{
    Iso15765Config kernel;
    RawCanConfig bootloader;
};

class IMixedCanFlashTransport : public IFlashTransport
{
  public:
    virtual Status configure(const MixedCanConfig&) = 0;
    virtual Status open() = 0;
    virtual Status close() = 0;
    virtual Status enter_raw_bootloader_mode() = 0;
    virtual Status clear_receive_buffer() = 0;
    virtual Status enter_iso15765_kernel_mode() = 0;
    virtual Status write_iso15765(bytes::ByteView, const ICancellationToken&) = 0;
    virtual Result<std::optional<bytes::Bytes>> read_iso15765(int, const ICancellationToken&) = 0;
    virtual Status write_raw(const cdbg::CanFrame&, const ICancellationToken&) = 0;
    virtual Result<std::optional<cdbg::CanFrame>> read_raw(int, const ICancellationToken&) = 0;
};

class IMixedCanFlashExecutor
{
  public:
    using TransportType = IMixedCanFlashTransport;
    using ConfigType = MixedCanConfig;
    virtual ~IMixedCanFlashExecutor() = default;
    virtual Result<MixedCanConfig> transport_setup(const FlashPlan&) const = 0;
    virtual Status before_transport_open(const ICancellationToken&) const { return {}; }
    virtual Result<FlashExecutionResult> execute(const FlashPlan&, IMixedCanFlashTransport&, IClock&,
                                                 const ICancellationToken&, IEventSink&) = 0;
};
```

Add `TransportKind::CanRawIso15765`; do not add `CanRaw`. Include `src/backend/protocol/ican_transport.h` for the shared raw-frame value.

Add only the transport kind in this task. Each family enum, `FamilyPlan` alternative, validation switch arm, and experimental ID lands atomically with that family's type in Tasks 3, 5, 7, and 8.

Expand `family_cases()` from seven to all 15 pre-Wave-5 alternatives before enabling the size assertion. Add rows for `SubaruHitachiM32rCanPlan`, the three existing CVT CAN plans, and the four Wave-4 Denso CAN plans, using their declared transport and `FlashPlan::experimental_family_id()` strings. This makes every later enum/variant addition fail compilation until its registry row and both exhaustive switches are updated.

- [ ] **Step 3: Add a scripted mixed transport and its failing behavior tests**

```cpp
enum class ScriptedMixedCanMode { Unconfigured, Iso15765Kernel, RawBootloader, Closed };

ScriptedMixedCanFlashTransport transport;
transport.expectIsoWrite({0x00, 0x00, 0x07, 0xE0, 0xBE, 0xEF});
transport.queueIsoRead({0x00, 0x00, 0x07, 0xE8, 0xBE, 0xEF, 0x00, 0x00, 0x41});
transport.expectRawWrite(cdbg::CanFrame{0x000FFFFE, {0x7A, 0x90, 0, 0, 0, 0, 0, 0}});
EXPECT_THAT(transport.modeChanges(), ElementsAre(ScriptedMixedCanMode::Iso15765Kernel,
                                                  ScriptedMixedCanMode::RawBootloader));
```

The fake must expose `expectIsoWrite`, `queueIsoRead`, `expectRawWrite`, `queueRawRead`, `queueNoIsoFrame`, `queueNoRawFrame`, `queueIsoError`, `queueRawError`, `failNextRawTransition`, `failNextIsoTransition`, `scriptConsumed`, `modeChanges`, `configureCallCount`, `openCallCount`, `closeCallCount`, and cancellation-aware blocking reads released by `request_unblock()`.

Run: `bazel test --config=release //src/backend/flash/testing:scripted_flash_transports_test`

Expected: FAIL until the fake records and validates both frame domains separately.

- [ ] **Step 4: Implement the scripted transport and make both suites pass**

Implement separate ISO and raw expectation queues; reject a method called in the wrong mode with `InvalidConfig`; reject payloads over eight bytes with `InvalidConfig`; return `Cancelled` before consuming a script item when the token or unblock flag is set.

Run: `bazel test --config=release //src/backend/flash:bound_flash_attempt_test //src/backend/flash:flash_validation_test //src/backend/flash/testing:scripted_flash_transports_test //src/backend/flash:flash_types_test`

Expected: PASS.

- [ ] **Step 5: Commit the portable seam**

```bash
git add src/backend/flash/bound_flash_attempt_test.cpp src/backend/flash/flash_executor.h \
  src/backend/flash/flash_types.h src/backend/flash/flash_types_test.cpp src/backend/flash/flash_validation_test.cpp \
  src/backend/flash/testing/scripted_mixed_can_flash_transport.h \
  src/backend/flash/testing/scripted_flash_transports_test.cpp src/backend/flash/testing/BUILD.bazel
git commit -m "feat(flash): add mixed CAN transport capability"
```

### Task 2: Desktop mixed-CAN adapter

**Files:**
- Create: `src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h`
- Create: `src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.cpp`
- Create: `src/platform/desktop/common/transport/desktop_mixed_can_flash_transport_test.cpp`
- Modify: `src/platform/desktop/common/transport/BUILD.bazel`
- Modify: `src/platform/desktop/common/serial/testing/fake_backend.h`

**Interfaces:**
- Consumes: `IMixedCanFlashTransport`, `MixedCanConfig`, non-owning/owning `SerialPortActions` construction pattern from `DesktopCanFlashTransport`.
- Produces: `DesktopMixedCanFlashTransport(std::unique_ptr<SerialPortActions>)` and `DesktopMixedCanFlashTransport(SerialPortActions*)`.

- [ ] **Step 1: Write configuration, transition, framing, and error tests**

```cpp
TEST(DesktopMixedCanFlashTransport, ConfiguresIsoThenTransitionsRawAndBack)
{
    ASSERT_TRUE(transport.configure(config()).has_value());
    ASSERT_TRUE(transport.open().has_value());
    ASSERT_TRUE(transport.enter_raw_bootloader_mode().has_value());
    ASSERT_TRUE(transport.enter_iso15765_kernel_mode().has_value());
    EXPECT_THAT(fake->takeCallLog(), ElementsAre(
        "cfg:set_is_iso14230_connection:0", "cfg:set_is_can_connection:0",
        "cfg:set_is_iso15765_connection:1", "cfg:set_is_29_bit_id:0",
        "cfg:set_can_speed:500000", "cfg:set_can_source_address:1048574",
        "cfg:set_can_destination_address:33", "cfg:set_iso15765_source_address:2016",
        "cfg:set_iso15765_destination_address:2024", "open_serial_port",
        "reset_connection", "cfg:set_is_iso14230_connection:0",
        "cfg:set_is_can_connection:1", "cfg:set_is_iso15765_connection:0",
        "cfg:set_is_29_bit_id:1", "cfg:set_can_speed:500000",
        "cfg:set_can_source_address:1048574", "cfg:set_can_destination_address:33",
        "open_serial_port", "reset_connection", "cfg:set_is_iso14230_connection:0",
        "cfg:set_is_can_connection:0", "cfg:set_is_iso15765_connection:1",
        "cfg:set_is_29_bit_id:0", "cfg:set_can_speed:500000",
        "cfg:set_can_source_address:1048574", "cfg:set_can_destination_address:33",
        "cfg:set_iso15765_source_address:2016", "cfg:set_iso15765_destination_address:2024",
        "open_serial_port"));
}

TEST(DesktopMixedCanFlashTransport, RawFrameAddsAndParsesBigEndianId)
{
    ASSERT_TRUE(transport.write_raw({0x000FFFFE, {0x7A, 0x90, 0, 0, 0, 0, 0, 0}}, token).has_value());
    EXPECT_THAT(fake->takeCallLog(), Contains("write_echo_check:begin:000ffffe7a90000000000000"));
    fake->scriptedResponse = QByteArray::fromHex("000000217a96000000000000");
    auto frame = transport.read_raw(800, token);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(frame->has_value());
    EXPECT_EQ(frame->value().id, 0x21U);
    EXPECT_EQ(frame->value().payload, (bytes::Bytes{0x7A,0x96,0,0,0,0,0,0}));
}
```

Also test every failed setter, failed reopen, short raw response (`BadResponse`), wrong receive ID (`BadResponse`), clear-buffer nonzero (`Internal`), disconnection before/during I/O, standard/nonstandard exceptions, cancellation, unblock suppression, and non-owning close safety.

Run: `bazel test --config=release //src/platform/desktop/common/transport:test_desktop_mixed_can_flash_transport`

Expected: FAIL because the adapter target does not exist.

- [ ] **Step 2: Extend `FakeBackend` only with the observable clear-buffer seam**

```cpp
int clearRxBufferResult = STATUS_SUCCESS;
int clear_rx_buffer() override
{
    log("clear_rx_buffer");
    return clearRxBufferResult;
}
```

Do not add protocol decisions to the fake backend.

- [ ] **Step 3: Implement the adapter**

```cpp
Status DesktopMixedCanFlashTransport::enter_raw_bootloader_mode()
{
    if (const Status reset = reset_connection(); !reset) return reset;
    if (const Status configured = configure_raw(stored_config_.bootloader); !configured) return configured;
    return open();
}

Status DesktopMixedCanFlashTransport::write_raw(const cdbg::CanFrame& frame,
                                                const ICancellationToken& cancellation)
{
    if (frame.payload.size() > 8) return fail(ErrorKind::InvalidConfig, "raw CAN payload exceeds 8 bytes");
    bytes::Bytes wire = bytes::composeBe(frame.id, frame.payload);
    return write_serial(wire, cancellation);
}
```

Initial `configure()` stores both configs and applies ISO mode. Both transition methods call `reset_connection()`, apply all relevant flags/addresses, and reopen. ISO read/write preserve the current `DesktopCanFlashTransport` behavior; raw read/write add/remove the four-byte ID prefix. Any failed transition is returned immediately and leaves the adapter unusable for further I/O.

- [ ] **Step 4: Run adapter and existing transport regression tests**

Run: `bazel test --config=release //src/platform/desktop/common/transport:test_desktop_mixed_can_flash_transport //src/platform/desktop/common/transport:test_desktop_can_flash_transport //src/platform/desktop/common/transport:test_desktop_kline_flash_transport`

Expected: PASS.

- [ ] **Step 5: Commit the adapter**

```bash
git add src/platform/desktop/common/transport src/platform/desktop/common/serial/testing/fake_backend.h
git commit -m "feat(desktop): adapt mixed CAN flash transport"
```

### Task 3: DensoCAN plan and executor

**Files:**
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_densocan_types.h`
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h`
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.cpp`
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan_test.cpp`
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.h`
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.cpp`
- Create: `src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`
- Modify: `src/backend/flash/flash_plan.cpp`
- Modify: `src/backend/flash/flash_types.h`
- Modify: `src/backend/flash/flash_validation.cpp`
- Modify: `src/backend/flash/flash_validation_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`

**Interfaces:**
- Consumes: `IMixedCanFlashExecutor`, `ScriptedMixedCanFlashTransport`, `SsmProtocol::calculatePayload`, `checksum::crc32`, `flashdevices[]`.
- Produces: `SubaruDensoSh705xDensoCanPlan`; `build_subaru_denso_sh705x_densocan_plan(FlashOperation, std::string_view, std::string_view, std::optional<bytes::Bytes>, KernelImage) -> Result<FlashPlan>`; `validate_subaru_denso_sh705x_densocan_plan(const FlashPlan&) -> Status`; `SubaruDensoSh705xDensoCanExecutor final : IMixedCanFlashExecutor`.

- [ ] **Step 1: Write exhaustive failing plan tests**

```cpp
struct Case { std::string_view protocol; std::string_view mcu; std::size_t rom_size; std::uint32_t kernel; };
constexpr Case kCases[] = {
    {"sub_ecu_denso_sh7055_densocan", "SH7055", 0x80000, 0xFFFF6004},
    {"sub_ecu_denso_sh7058_densocan", "SH7058", 0x100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7058s_densocan", "SH7058", 0x100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7058s_diesel_densocan", "SH7058", 0x100000, 0xFFFF3000},
    {"sub_ecu_denso_sh7059_diesel_densocan", "SH7059d", 0x180000, 0xFFFEE000},
};
```

For every case, test all three operations, exact ROM sizing, exact MCU pairing, full geometry, kernel bounds including padded six-byte upload length, the single `CycleIgnition` plan confirmation, mixed config values, and `TransportKind::CanRawIso15765`. Reject unknown and near-miss protocols before any transport exists.

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_densocan_plan_test`

Expected: FAIL because the target does not exist.

- [ ] **Step 2: Implement the DensoCAN type, builder, and validator**

```cpp
struct SubaruDensoSh705xDensoCanPlan
{
    std::uint32_t iso_request_id{0x7E0};
    std::uint32_t iso_response_id{0x7E8};
    std::uint32_t raw_transmit_id{0x000FFFFE};
    std::uint32_t raw_receive_id{0x21};
    int bitrate{500000};
    bool iso_extended_id{false};
    bool raw_extended_id{true};
};
```

Use an exact catalog table with the five cases above. Set `family=SubaruDensoSh705xDensoCan`, `transport=CanRawIso15765`, and exactly one `CycleIgnition` confirmation. Validate `kernel.load_address` against the catalog and ensure `round_up(kernel.bytes.size(), 6)` remains inside the selected device's kernel region. For write/test-write copy all 16 device blocks into `erase_regions`; for read use an empty erase list and the full-ROM `transfer_region`.

Register the type atomically across the closed-world plan machinery: add `FlashFamily::SubaruDensoSh705xDensoCan`, include the new types header, append `SubaruDensoSh705xDensoCanPlan` to `FamilyPlan`, add the root `flash_types` dependency, append the 16th `family_cases()` row, and add these two exhaustive switch arms:

```cpp
// flash_validation.cpp
case FlashFamily::SubaruDensoSh705xDensoCan:
    return fields.transport == TransportKind::CanRawIso15765 &&
           std::holds_alternative<SubaruDensoSh705xDensoCanPlan>(fields.family_plan);

// flash_plan.cpp
case FlashFamily::SubaruDensoSh705xDensoCan:
    return "SubaruDensoSh705xDensoCan";
```

Run: `bazel test --config=release //src/backend/flash:flash_validation_test //src/backend/flash/ecu:subaru_denso_sh705x_densocan_plan_test`

Expected: PASS; the registry-size assertion proves the new variant is represented in the exhaustive cross-product test.

- [ ] **Step 3: Write byte-exact failing executor tests**

Cover these separate traces:

```text
already-running/read:
  ISO BEEF kernel-id request -> valid kernel-id -> page reads -> decrypted ROM

upload/read:
  ISO BEEF probe timeout -> enter raw -> 1000 x [id=0x000FFFFE,payload=FF 86 00 00 00 00 00 00]
  -> clear -> raw 7A 90 -> 7A 96 -> raw 7A 9C + address
  -> six-byte kernel blocks -> raw 7A B4 + end+1 -> 7A B1
  -> raw 7A 9C + address -> raw 7A A0 -> enter ISO -> BEEF kernel-id -> page reads

write/test-write:
  kernel path -> per-block CRC comparison -> flash enable/init -> erase only for Write
  -> block transfer/commit/CRC -> flash disable; TestWrite emits the legacy checks without erase/program
```

Test both kernel paths, five geometry cases, exact raw IDs/payloads, six-byte final padding/checksum, `0x400` reads, every response-length guard, transition failures, wake/upload cancellation, read/write cancellation, timeout, disconnect, malformed response, tolerant jump response, logs, progress phases, and close precedence through `BoundAttempt`.

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_densocan_executor_test`

Expected: FAIL because the executor does not exist.

- [ ] **Step 4: Implement the executor from the cited legacy methods**

```cpp
Result<FlashExecutionResult> SubaruDensoSh705xDensoCanExecutor::execute(
    const FlashPlan& plan, IMixedCanFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (const Status valid = validate_subaru_denso_sh705x_densocan_plan(plan); !valid)
        return std::unexpected(valid.error());
    Result<bool> kernel_alive = probe_kernel(transport, cancellation, events);
    if (!kernel_alive) return std::unexpected(kernel_alive.error());
    if (!*kernel_alive)
    {
        if (Status s = transport.enter_raw_bootloader_mode(); !s) return std::unexpected(s.error());
        if (Status s = wake_bootloader(transport, clock, cancellation, events); !s)
            return std::unexpected(s.error());
        if (Status s = upload_kernel(transport, *plan.kernel(), clock, cancellation, events); !s)
            return std::unexpected(s.error());
        if (Status s = transport.enter_iso15765_kernel_mode(); !s) return std::unexpected(s.error());
        if (Status s = require_kernel_id(transport, cancellation, events); !s)
            return std::unexpected(s.error());
    }
    return plan.operation() == FlashOperation::Read
        ? execute_read(plan, transport, clock, cancellation, events)
        : execute_write(plan, transport, clock, cancellation, events);
}
```

Spell out helpers corresponding one-for-one to legacy `connect_bootloader` (106-222), `upload_kernel` (227-507), `read_mem` (512-657), `write_mem` (662-778), CRC/init/reflash/flash (783-1395), payload crypto (1400-1429), and kernel ID (1435-1484). Do not pass `BEEF` traffic to `UdsClient`.

- [ ] **Step 5: Run the portable DensoCAN tests**

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_densocan_plan_test //src/backend/flash/ecu:subaru_denso_sh705x_densocan_executor_test`

Expected: PASS with all scripts consumed.

- [ ] **Step 6: Commit the portable DensoCAN core**

```bash
git add src/backend/flash/BUILD.bazel src/backend/flash/flash_plan.cpp \
  src/backend/flash/flash_types.h src/backend/flash/flash_validation.cpp \
  src/backend/flash/flash_validation_test.cpp src/backend/flash/ecu
git commit -m "feat(flash): port DensoCAN core"
```

### Task 4: Route DensoCAN and remove its legacy family

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp`
- Modify: `src/platform/desktop/common/flash/flash_workflow_test.cpp`
- Modify: `src/platform/desktop/common/flash/BUILD.bazel`
- Modify: `src/ui/desktop/mainwindow.{h,cpp}`
- Modify: `src/ui/desktop/flash/ecu/BUILD.bazel`
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel`
- Delete: `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_densocan.{h,cpp}`
- Delete: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.{h,cpp}`
- Modify: `scripts/check-legacy-flash-drain.py`
- Modify: `docs/flash-qualification-matrix.md`
- Modify: `BUILD.bazel`
- Modify: `scripts/check-portable-closure.py`

**Interfaces:**
- Consumes: DensoCAN builder/executor, `DesktopMixedCanFlashTransport`, kernel resolution, `FlashAttemptOutcome`, `FlashDialog`.
- Produces: `KernelBackedCanFlashWorkflow<Executor, BuildPlan, Transport>` and five exact DensoCAN routes.

- [ ] **Step 1: Write failing exact-route and workflow tests**

```cpp
constexpr const char *kFiveDensoCanProtocols[] = {
    "sub_ecu_denso_sh7055_densocan",
    "sub_ecu_denso_sh7058_densocan",
    "sub_ecu_denso_sh7058s_densocan",
    "sub_ecu_denso_sh7058s_diesel_densocan",
    "sub_ecu_denso_sh7059_diesel_densocan",
};
for (const char *protocol : kFiveDensoCanProtocols)
    QVERIFY(FlashWorkflowFactory::tryCreate(request(protocol)) != nullptr);
for (const char *near_miss : {"sub_ecu_denso_sh7058_densocan_extra", "future_densocan"})
    QVERIFY(FlashWorkflowFactory::tryCreate(request(near_miss)) == nullptr);
```

Build a temporary catalog/kernel and prove step order is `Begin`, `CycleIgnition`, `FlashAttempt`; declining either prompt returns `Cancelled` before attempt construction. Assert the attempt plan owns the copied kernel bytes and has `CanRawIso15765`.

Run: `bazel test --config=release //src/platform/desktop/common/flash:test_flash_workflow`

Expected: FAIL because DensoCAN is not registered.

- [ ] **Step 2: Implement exact matching and the kernel-backed workflow**

```cpp
enum class RouteMatch { Prefix, Exact };
struct Route { std::string_view pattern; Route::Kind kind; RouteMatch match; };

bool matches(const Route& route, std::string_view protocol)
{
    return route.match == RouteMatch::Exact ? protocol == route.pattern
                                             : protocol.starts_with(route.pattern);
}

template <typename ExecutorT,
          Result<FlashPlan> (*BuildPlan)(FlashOperation, std::string_view, std::string_view,
                                         std::optional<bytes::Bytes>, KernelImage),
          typename TransportT>
class KernelBackedCanFlashWorkflow final : public FlashWorkflow;

using SubaruDensoSh705xDensoCanWorkflow =
    KernelBackedCanFlashWorkflow<SubaruDensoSh705xDensoCanExecutor,
                                 &build_subaru_denso_sh705x_densocan_plan,
                                 DesktopMixedCanFlashTransport>;
```

Keep all existing rows `Prefix`; add only the five DensoCAN rows as `Exact`. The workflow resolves `KernelImage` once through `QtFileRepository`, returns preflight failures before `Begin`, calls the typed builder, maps `ConfirmationSpec::Id::CycleIgnition` to `FlashPromptKind::CycleIgnition`, constructs `TransportT(request_.serial)`, and propagates both `read_bytes` and `rom_id` into `FlashCompletedStep`.

Use `FlashAttemptOutcome outcome_;` for submitted attempt results and terminal state, matching `SimpleCanFlashWorkflow`; do not add a second result-state implementation. Do not extend `SimpleCanFlashWorkflow`: its builder signature has no `KernelImage` and it hard-codes `DesktopCanFlashTransport`, so it cannot represent DensoCAN's mixed transport.

- [ ] **Step 3: Flip dispatch, delete the legacy pair, and tighten guards**

Remove the broad `endsWith("_densocan")` branch and its include from `mainwindow`. Delete the dialog and operation sources from their BUILD targets. Remove only `FlashEcuSubaruDensoSH705xDensoCan` from `REMAINING`, register both new portable Bazel roots, and update the matrix row to `portable=yes`, `experimental`, with mixed-transport and automated-evidence notes. Expected drain count: 13.

```cpp
{"sub_ecu_denso_sh7055_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
{"sub_ecu_denso_sh7058_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
{"sub_ecu_denso_sh7058s_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
{"sub_ecu_denso_sh7058s_diesel_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
{"sub_ecu_denso_sh7059_diesel_densocan", SubaruDensoSh705xDensoCan, RouteMatch::Exact},
```

- [ ] **Step 4: Prove guard changes are non-vacuous and run the family gate**

Run:

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_densocan_plan_test \
  //src/backend/flash/ecu:subaru_denso_sh705x_densocan_executor_test \
  //src/platform/desktop/common/transport:test_desktop_mixed_can_flash_transport \
  //src/platform/desktop/common/flash:test_flash_workflow \
  //:portable_closure //:serial_compat_allowlist //:legacy_flash_drain //:backend_no_widgets
```

Temporarily remove one new root from each guard's data/list and confirm that guard fails; restore it and rerun to PASS. Confirm `rg -n 'FlashEcuSubaruDensoSH705xDensoCan' src scripts` has no production hit.

- [ ] **Step 5: Commit the complete DensoCAN family**

```bash
git add BUILD.bazel scripts src docs/flash-qualification-matrix.md
git commit -m "feat(flash): route portable DensoCAN family"
```

### Task 5: Denso SH705x TCU portable core

**Files:**
- Create: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_types.h`
- Create: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan_test.cpp`
- Create: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`
- Modify: `src/backend/flash/flash_plan.cpp`
- Modify: `src/backend/flash/flash_types.h`
- Modify: `src/backend/flash/flash_validation.cpp`
- Modify: `src/backend/flash/flash_validation_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`

**Interfaces:**
- Consumes: `ICanFlashExecutor`, `CanFlashUdsChannel`, `uds::UdsClient`, SSM crypto/checksum, scripted CAN transport.
- Produces: `SubaruTcuDensoSh705xCanPlan`; `build_subaru_tcu_denso_sh705x_can_plan(FlashOperation, std::string_view, std::string_view, std::optional<bytes::Bytes>, KernelImage) -> Result<FlashPlan>`; `validate_subaru_tcu_denso_sh705x_can_plan(const FlashPlan&) -> Status`; `SubaruTcuDensoSh705xCanExecutor`.

- [ ] **Step 1: Write failing TCU plan tests**

```cpp
TEST(SubaruTcuDensoSh705xCanPlan, CapabilitiesAreProtocolSpecific)
{
    auto sh7055_read = build_subaru_tcu_denso_sh705x_can_plan(
        FlashOperation::Read, "sub_tcu_denso_sh7055_can", "SH7055", std::nullopt,
        KernelImage{.id="sh7055-test", .load_address=0xFFFF9000, .bytes={0x01,0x02,0x03,0x04}});
    ASSERT_TRUE(sh7055_read.has_value());

    auto sh7055_write = build_subaru_tcu_denso_sh705x_can_plan(
        FlashOperation::Write, "sub_tcu_denso_sh7055_can", "SH7055", bytes::Bytes(0x80000),
        KernelImage{.id="sh7055-test", .load_address=0xFFFF9000, .bytes={0x01,0x02,0x03,0x04}});
    ASSERT_FALSE(sh7055_write.has_value());
    EXPECT_EQ(sh7055_write.error().kind, ErrorKind::Unsupported);

    auto sh7058_write = build_subaru_tcu_denso_sh705x_can_plan(
        FlashOperation::Write, "sub_tcu_denso_sh7058_can", "SH7058", bytes::Bytes(0x100000),
        KernelImage{.id="sh7058-test", .load_address=0xFFFF3000, .bytes={0x01,0x02,0x03,0x04}});
    ASSERT_TRUE(sh7058_write.has_value());

    auto test_write = build_subaru_tcu_denso_sh705x_can_plan(
        FlashOperation::TestWrite, "sub_tcu_denso_sh7058_can", "SH7058", bytes::Bytes(0x100000),
        KernelImage{.id="sh7058-test", .load_address=0xFFFF3000, .bytes={0x01,0x02,0x03,0x04}});
    ASSERT_FALSE(test_write.has_value());
    EXPECT_EQ(test_write.error().kind, ErrorKind::Unsupported);
}
```

Also pin kernel addresses `0xFFFF9000`/`0xFFFF3000`, ROM sizes, 16-block geometries, ISO config `0x7E1/0x7E9`, full transfer region, absence of extra confirmations, wrong-MCU rejection, image sizing, and near misses.

- [ ] **Step 2: Implement TCU types/builder/validator**

```cpp
struct SubaruTcuDensoSh705xCanPlan
{
    std::uint32_t request_id{0x7E1};
    std::uint32_t response_id{0x7E9};
    int bitrate{500000};
    bool extended_id{false};
};
```

The exact protocol table supplies MCU, ROM size, kernel address, and capability flags. Return `Unsupported` before `validate_and_build` for SH7055 write and every test-write.

Register the type atomically: add `FlashFamily::SubaruTcuDensoSh705xCan`, include/append `SubaruTcuDensoSh705xCanPlan` in `FamilyPlan`, add the root `flash_types` dependency, append the 17th `family_cases()` row, and add:

```cpp
// flash_validation.cpp
case FlashFamily::SubaruTcuDensoSh705xCan:
    return fields.transport == TransportKind::CanIso15765 &&
           std::holds_alternative<SubaruTcuDensoSh705xCanPlan>(fields.family_plan);

// flash_plan.cpp
case FlashFamily::SubaruTcuDensoSh705xCan:
    return "SubaruTcuDensoSh705xCan";
```

Run: `bazel test --config=release //src/backend/flash:flash_validation_test //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_plan_test`

Expected: PASS with the registry-size assertion still equal to `FamilyPlan`'s alternative count.

- [ ] **Step 3: Write failing byte-exact TCU executor tests**

Test kernel-alive and upload paths, read for both MCUs, SH7058 write, plan rejection before I/O, exact UDS session/security/download exchanges, the initial ISO-15765 `00 00 07 E1 7A A0 00 00 00 00 00 00` kernel probe, subsequent proprietary `BEEF` kernel exchanges on `0x7E1/0x7E9`, identity/ROM-ID handling, CRC comparison, erase/program ordering, timeouts, tolerant legacy replies, malformed replies, disconnect, and cancellation at every loop boundary.

Run: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test`

Expected: FAIL because the executor does not exist.

- [ ] **Step 4: Implement the TCU executor**

```cpp
class SubaruTcuDensoSh705xCanExecutor final : public ICanFlashExecutor
{
  public:
    Result<Iso15765Config> transport_setup(const FlashPlan&) const override;
    Result<FlashExecutionResult> execute(const FlashPlan&, ICanFlashTransport&, IClock&,
                                         const ICancellationToken&, IEventSink&) override;
};
```

Map helpers one-for-one to legacy `connect_bootloader` (93-364), `upload_kernel` (369-627), `read_mem` (632-779), `write_mem` (784-900), CRC/init/reflash/flash (905-1517), crypto (1522-1577), and kernel-ID request (1583-1647). Use `UdsClient` only for exchanges with strict UDS response semantics; preserve nonfatal identity-query behavior.

- [ ] **Step 5: Run and commit the portable TCU core**

```bash
bazel test --config=release //src/backend/flash:flash_validation_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_plan_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
git add src/backend/flash/BUILD.bazel src/backend/flash/flash_plan.cpp \
  src/backend/flash/flash_types.h src/backend/flash/flash_validation.cpp \
  src/backend/flash/flash_validation_test.cpp src/backend/flash/ecu
git commit -m "feat(flash): port Denso SH705x TCU core"
```

### Task 6: Preserve the TCU chooser, route Dump, and remove legacy TCU flash

**Files:**
- Create: `src/ui/desktop/service_functions/denso_tcu_read_preflight.{h,cpp}`
- Create: `src/ui/desktop/service_functions/denso_tcu_read_preflight_test.cpp`
- Modify: `src/ui/desktop/service_functions/BUILD.bazel`
- Modify: `src/ui/desktop/mainwindow.{h,cpp}`
- Modify: `src/ui/desktop/BUILD.bazel`
- Modify: `src/platform/desktop/common/flash/flash_workflow.{cpp}`
- Modify: `src/platform/desktop/common/flash/flash_workflow_test.cpp`
- Modify: `src/platform/desktop/common/flash/BUILD.bazel`
- Delete: `src/ui/desktop/flash/tcu/flash_tcu_subaru_denso_sh705x_can.{h,cpp}`
- Delete: `src/ui/desktop/flash/tcu/flash_tcu_subaru_denso_sh705x_can_test.cpp`
- Delete: `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_denso_sh705x_can_operation.{h,cpp}`
- Modify: `src/ui/desktop/flash/tcu/BUILD.bazel`
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel`
- Modify: `scripts/check-legacy-flash-drain.py`
- Modify: `docs/flash-qualification-matrix.md`
- Modify: `BUILD.bazel`
- Modify: `scripts/check-portable-closure.py`

**Interfaces:**
- Consumes: `ServiceFunctionDialog`, `ServiceFunctionKind`, TCU builder/executor, `KernelBackedCanFlashWorkflow`.
- Produces: `DensoTcuReadAction`; `choose_denso_tcu_read_action(QWidget*) -> DensoTcuReadAction`; and `run_denso_tcu_service_action(DensoTcuReadAction, SerialPortActions*, std::string, QWidget*) -> bool`, where `true` means MainWindow must return without creating a flash workflow.

- [ ] **Step 1: Write the four-choice and cancellation tests**

```cpp
enum class DensoTcuReadAction { Dump, Relearn, ReadParameters, SetParameters, Cancelled };

QTest::newRow("dump") << "Dump" << static_cast<int>(DensoTcuReadAction::Dump);
QTest::newRow("relearn") << "Relearn" << static_cast<int>(DensoTcuReadAction::Relearn);
QTest::newRow("read") << "Read Param" << static_cast<int>(DensoTcuReadAction::ReadParameters);
QTest::newRow("set") << "Set Param" << static_cast<int>(DensoTcuReadAction::SetParameters);
```

Prove Dump returns to the caller without showing the helper's ignition prompt; each service action shows the existing ignition prompt then opens the matching `ServiceFunctionDialog`; closing the chooser or declining ignition returns without serial calls. Keep the existing exact button strings, including `Read Param` and `Set Param`.

- [ ] **Step 2: Implement the preflight helper**

```cpp
DensoTcuReadAction choose_denso_tcu_read_action(QWidget *parent);

std::optional<ServiceFunctionKind> to_service_kind(DensoTcuReadAction action)
{
    switch (action)
    {
    case DensoTcuReadAction::Relearn: return ServiceFunctionKind::Relearn;
    case DensoTcuReadAction::ReadParameters: return ServiceFunctionKind::ReadParameters;
    case DensoTcuReadAction::SetParameters: return ServiceFunctionKind::SetParameters;
    case DensoTcuReadAction::Dump:
    case DensoTcuReadAction::Cancelled: return std::nullopt;
    }
    return std::nullopt;
}

bool confirm_tcu_ignition(QWidget *parent)
{
    return QMessageBox::warning(parent, QObject::tr("Connecting to TCU"),
        QObject::tr("Turn ignition ON and press OK to start initializing connection to TCU"),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok) == QMessageBox::Ok;
}

bool run_denso_tcu_service_action(DensoTcuReadAction action, SerialPortActions *serial,
                                  std::string protocol, QWidget *parent)
{
    if (action == DensoTcuReadAction::Dump) return false;
    if (action == DensoTcuReadAction::Cancelled) return true;
    if (!confirm_tcu_ignition(parent)) return true;
    ServiceFunctionDialog dialog{serial, std::move(protocol), *to_service_kind(action), parent};
    dialog.exec();
    return true;
}
```

Keep chooser and ignition on the GUI thread; do not configure serial in this helper.

- [ ] **Step 3: Move chooser dispatch before factory creation**

```cpp
const bool denso_tcu = protocol == "sub_tcu_denso_sh7055_can" ||
                       protocol == "sub_tcu_denso_sh7058_can";
if (operation == FlashOperation::Read && denso_tcu)
{
    const auto action = choose_denso_tcu_read_action(this);
    if (run_denso_tcu_service_action(action, serial, protocol, this)) return;
}
auto workflow = FlashWorkflowFactory::tryCreate({
    .operation = operation,
    .protocol = protocol,
    .mcu = ecuCalDef[rom_number]->McuType.toStdString(),
    .image = std::move(portable_image),
    .paths = eeprom_paths,
    .display_filename = ecuCalDef[rom_number]->FileName.toStdString(),
    .serial = serial,
});
```

Add both exact TCU routes to the factory. Dump reaches `FlashDialog`, whose `Begin` prompt is the ignition gate; write bypasses the chooser; unsupported operations fail plan validation before I/O.

- [ ] **Step 4: Delete legacy TCU flash and update guards/docs**

Remove both broad TCU legacy branches and the old include. Delete the wrapper/operation/test and BUILD entries. Remove `FlashTcuSubaruDensoSH705xCan` from `REMAINING`; expected drain count: 12. Register both new portable targets and update the matrix row to experimental automated coverage.

- [ ] **Step 5: Run TCU desktop, workflow, and guard tests**

Run:

```bash
bazel test --config=release //src/ui/desktop/service_functions:test_denso_tcu_read_preflight \
  //src/platform/desktop/common/flash:test_flash_workflow \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_plan_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test \
  //:portable_closure //:serial_compat_allowlist //:legacy_flash_drain //:backend_no_widgets
```

Expected: PASS. Temporarily break each new guard registration, observe failure, restore, and rerun.

- [ ] **Step 6: Commit the complete TCU family**

```bash
git add BUILD.bazel scripts src docs/flash-qualification-matrix.md
git commit -m "feat(flash): route portable Denso TCU family"
```

### Task 7: SH7058 petrol ECU family

**Files:**
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_types.h`
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_plan.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_plan_test.cpp`
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`
- Modify: `src/backend/flash/flash_plan.cpp`
- Modify: `src/backend/flash/flash_types.h`
- Modify: `src/backend/flash/flash_validation.cpp`
- Modify: `src/backend/flash/flash_validation_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Modify: `src/platform/desktop/common/flash/flash_workflow.{cpp}`
- Modify: `src/platform/desktop/common/flash/flash_workflow_test.cpp`
- Modify: `src/platform/desktop/common/flash/BUILD.bazel`
- Modify: `src/ui/desktop/mainwindow.{h,cpp}`
- Delete: `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh7058_can.{h,cpp}`
- Delete: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7058_can_operation.{h,cpp}`
- Modify: `src/ui/desktop/flash/ecu/BUILD.bazel`
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel`
- Modify: `scripts/check-legacy-flash-drain.py`
- Modify: `docs/flash-qualification-matrix.md`
- Modify: `BUILD.bazel`
- Modify: `scripts/check-portable-closure.py`

**Interfaces:**
- Produces: `SubaruDensoSh7058CanSecurity`; `SubaruDensoSh7058CanPlan`; `build_subaru_denso_sh7058_can_plan(FlashOperation, std::string_view, std::string_view, std::optional<bytes::Bytes>, KernelImage) -> Result<FlashPlan>`; `validate_subaru_denso_sh7058_can_plan(const FlashPlan&) -> Status`; `SubaruDensoSh7058CanExecutor : ICanFlashExecutor`.

- [ ] **Step 1: Write plan tests for all five exact variants**

```cpp
constexpr std::pair<std::string_view, SubaruDensoSh7058CanSecurity> kVariants[] = {
    {"sub_ecu_denso_sh7058_can", Stock},
    {"sub_ecu_denso_sh7058_can_ecutek", EcuTek},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", RaceRom},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", RaceRomAlt},
    {"sub_ecu_denso_sh7058_can_cobb", Cobb},
};
```

For every row and operation, pin `SH7058`, `0x100000`, kernel `0xFFFF3000`, exact 16-block geometry, ISO config, no extra confirmation, exact image sizing, and near-miss rejection.

- [ ] **Step 2: Implement types/builder/validator and run plan tests**

The plan struct holds ISO config plus the security enum. The executor must switch on that enum; it must never inspect `plan.target_id()` suffixes.

```cpp
struct SubaruDensoSh7058CanPlan
{
    std::uint32_t request_id{0x7E0};
    std::uint32_t response_id{0x7E8};
    int bitrate{500000};
    bool extended_id{false};
    SubaruDensoSh7058CanSecurity security;
};
```

In the same implementation step, register `FlashFamily::SubaruDensoSh7058Can`, include/append `SubaruDensoSh7058CanPlan` in `FamilyPlan`, add the root `flash_types` dependency, append the 18th `family_cases()` row, and add:

```cpp
// flash_validation.cpp
case FlashFamily::SubaruDensoSh7058Can:
    return fields.transport == TransportKind::CanIso15765 &&
           std::holds_alternative<SubaruDensoSh7058CanPlan>(fields.family_plan);

// flash_plan.cpp
case FlashFamily::SubaruDensoSh7058Can:
    return "SubaruDensoSh7058Can";
```

Run: `bazel test --config=release //src/backend/flash:flash_validation_test //src/backend/flash/ecu:subaru_denso_sh7058_can_plan_test`

Expected: PASS with all 18 plan alternatives present in the registry cross-product.

- [ ] **Step 3: Write fixed-vector security and byte-exact executor tests**

```cpp
struct SecurityVector
{
    SubaruDensoSh7058CanSecurity security;
    std::array<bytes::Byte, 4> expected_key;
};

constexpr SecurityVector kSecurityVectors[] = {
    {SubaruDensoSh7058CanSecurity::Stock,   {0x35,0xB6,0x83,0xBF}},
    {SubaruDensoSh7058CanSecurity::EcuTek,  {0xAD,0xD9,0x60,0xEE}},
    {SubaruDensoSh7058CanSecurity::RaceRom, {0x05,0x88,0x2B,0x0C}},
    {SubaruDensoSh7058CanSecurity::Cobb,    {0x2C,0xD1,0x8A,0xEE}},
};
// Seed 11 22 33 44. RaceRomAlt additionally scripts CAL ID AE5Z500V,
// seed-alter RAM 01 02 03 04, and XOR RAM 00 00 12 34; expected key FF 90 1D D4.
```

Independently calculate and hard-code expected keys for all five variants. Cover both RaceRom branches and the legacy RAM-derived alteration values, kernel-alive/upload paths, read/test-write/write, UDS vs tolerant exchanges, ROM ID, CRC/erase/program flow, response errors, retries, timeouts, disconnect, cancellation, logs, and progress.

- [ ] **Step 4: Implement from the petrol legacy source**

Map helpers to `connect_bootloader` (112-621), `upload_kernel` (626-870), `read_mem` (875-1022), `write_mem` (1027-1143), CRC/init/reflash/flash (1148-1760), security (1765-1906), crypto (1911-1940), and kernel ID (1946-1996). Preserve the legacy tolerance of nonfatal ECU/VIN/CAL/CVN queries and use UDS only where response rules actually match.

- [ ] **Step 5: Route exact IDs, delete legacy, update guards and verify**

Add five `Exact` routes through `KernelBackedCanFlashWorkflow`; remove the broad petrol legacy branch and include; delete the wrapper/operation sources and BUILD entries. Shrink the drain to 11, register portable targets, and update the matrix.

Run:

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh7058_can_plan_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/platform/desktop/common/flash:test_flash_workflow \
  //:portable_closure //:serial_compat_allowlist //:legacy_flash_drain //:backend_no_widgets
```

Expected: PASS; each security variant and near miss is exercised.

- [ ] **Step 6: Commit the petrol family**

```bash
git add BUILD.bazel scripts src docs/flash-qualification-matrix.md
git commit -m "feat(flash): port Denso SH7058 CAN petrol family"
```

### Task 8: SH7058/SH7059 diesel ECU family

**Files:**
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h`
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan_test.cpp`
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.{h,cpp}`
- Create: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`
- Modify: `src/backend/flash/flash_plan.cpp`
- Modify: `src/backend/flash/flash_types.h`
- Modify: `src/backend/flash/flash_validation.cpp`
- Modify: `src/backend/flash/flash_validation_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp`
- Modify: `src/platform/desktop/common/flash/flash_workflow_test.cpp`
- Modify: `src/platform/desktop/common/flash/BUILD.bazel`
- Modify: `src/ui/desktop/mainwindow.{h,cpp}`
- Delete: `src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh7058_can_diesel.{h,cpp}`
- Delete: `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.{h,cpp}`
- Modify: `src/ui/desktop/flash/ecu/BUILD.bazel`
- Modify: `src/platform/desktop/common/flash/legacy/BUILD.bazel`
- Modify: `scripts/check-legacy-flash-drain.py`
- Modify: `docs/flash-qualification-matrix.md`
- Modify: `BUILD.bazel`
- Modify: `scripts/check-portable-closure.py`

**Interfaces:**
- Produces: `SubaruDensoSh7058CanDieselPlan`; `build_subaru_denso_sh7058_can_diesel_plan(FlashOperation, std::string_view, std::string_view, std::optional<bytes::Bytes>, KernelImage) -> Result<FlashPlan>`; `validate_subaru_denso_sh7058_can_diesel_plan(const FlashPlan&) -> Status`; `SubaruDensoSh7058CanDieselExecutor : ICanFlashExecutor`.

- [ ] **Step 1: Write the two-variant plan tests**

```cpp
struct DieselCase
{
    std::string_view protocol;
    std::string_view mcu;
    std::size_t rom_size;
    std::uint32_t kernel_address;
};

constexpr DieselCase kDiesel[] = {
    {"sub_ecu_denso_sh7058_can_diesel", "SH7058d", 0x100000, 0xFFFF4000},
    {"sub_ecu_denso_sh7059_can_diesel", "SH7059d", 0x180000, 0xFFFEE000},
};
```

Pin all operations, exact pairing, complete 16-block geometry, kernel bounds, ISO config, full transfer/erase regions, no extra confirmation, wrong-size image rejection, and near misses.

- [ ] **Step 2: Implement and verify builder/validator**

Use the exact table above and copy the selected `flashdevices[]` blocks. Do not infer diesel generation from a suffix after exact selection.

```cpp
struct SubaruDensoSh7058CanDieselPlan
{
    std::uint32_t request_id{0x7E0};
    std::uint32_t response_id{0x7E8};
    int bitrate{500000};
    bool extended_id{false};
};
```

In the same implementation step, register `FlashFamily::SubaruDensoSh7058CanDiesel`, include/append `SubaruDensoSh7058CanDieselPlan` in `FamilyPlan`, add the root `flash_types` dependency, append the 19th `family_cases()` row, and add:

```cpp
// flash_validation.cpp
case FlashFamily::SubaruDensoSh7058CanDiesel:
    return fields.transport == TransportKind::CanIso15765 &&
           std::holds_alternative<SubaruDensoSh7058CanDieselPlan>(fields.family_plan);

// flash_plan.cpp
case FlashFamily::SubaruDensoSh7058CanDiesel:
    return "SubaruDensoSh7058CanDiesel";
```

Run: `bazel test --config=release //src/backend/flash:flash_validation_test //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_plan_test`

Expected: PASS with all 19 plan alternatives represented.

- [ ] **Step 3: Write byte-exact executor tests for both generations**

Cover stock seed fixed vectors, kernel-alive/upload, SH7058d and SH7059d address/length differences, read/test-write/write, identity queries, diagnostic-session fallback, upload padding/checksum/encryption, kernel CRC, flash initialization, erase/program, timeout, malformed/negative response, disconnect, cancellation, logs, and progress.

```cpp
for (const DieselCase& diesel : kDiesel)
{
    SCOPED_TRACE(diesel.protocol);
    auto plan = build_subaru_denso_sh7058_can_diesel_plan(
        FlashOperation::Read, diesel.protocol, diesel.mcu, std::nullopt,
        KernelImage{.id=std::string(diesel.protocol), .load_address=diesel.kernel_address,
                    .bytes={0x01,0x02,0x03,0x04}});
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->transfer_region().length, diesel.rom_size);
}
```

- [ ] **Step 4: Implement from the diesel legacy source**

Map helpers to `connect_bootloader` (113-512), `upload_kernel` (517-784), `read_mem` (789-938), `write_mem` (943-1059), CRC/init/reflash/flash (1064-1691), security/crypto (1696-1791), and kernel ID (1797-1840). Preserve SH7059d's distinct kernel/geometry/timing values and do not activate the unused EcuTek/Cobb helpers for protocols that do not select them.

- [ ] **Step 5: Route, delete legacy, update guards/docs, and verify**

Add two exact routes; remove both broad diesel branches and include; delete wrapper/operation sources and BUILD entries. Shrink the drain to 10, register roots, and update the matrix.

Run:

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_plan_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/platform/desktop/common/flash:test_flash_workflow \
  //:portable_closure //:serial_compat_allowlist //:legacy_flash_drain //:backend_no_widgets
```

Expected: PASS with `//:legacy_flash_drain` reporting exactly 10 families.

- [ ] **Step 6: Commit the diesel family**

```bash
git add BUILD.bazel scripts src docs/flash-qualification-matrix.md
git commit -m "feat(flash): port Denso SH705x CAN diesel family"
```

### Task 9: Cluster comparison, evidence-only factoring, and Wave 5 closeout

**Files:**
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp`
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.h`
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Modify: `docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md`
- Modify: `docs/modularization-plan.md`
- Modify: `docs/flash-qualification-matrix.md`

**Interfaces:**
- Consumes: all four passing portable implementations, their characterization tests, and the existing `kDensoIso15765SeedKeyTable`, `kDensoIso15765EncryptTable`, `kDensoIso15765DecryptTable`, and `kDensoIso15765IndexTransformation` constants.
- Produces: shared use of the applicable already-tested constants by the three ISO-only Wave 5 executors — all four for TCU/petrol, and seed/index/encrypt only for diesel because its BEEF reads remain raw; documented preservation of all other family-specific code; Wave 5 completion documentation.

- [ ] **Step 1: Compare only tested portable code**

Run:

```bash
git diff --no-index src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp
git diff --no-index src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp
rg -n "kDensoIso15765|request_kernel|encrypt|decrypt|CRC|retry|timeout" src/backend/flash/ecu/*sh705*can_executor.cpp
```

Record exact identical regions and exact differences in a commit-message note. Do not compare DensoCAN raw-mode functions as candidates for ISO-only factoring.

- [ ] **Step 2: Establish the green characterization baseline before changing dependencies**

The existing common test already pins seed `11 22 33 44` to key `35 B6 83 BF`, plus encryption and decryption vectors. Run it together with the three Wave 5 executor suites before editing includes or tables:

```bash
bazel test --config=release //src/backend/flash/ecu:denso_iso15765_can_common_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test
```

Expected: PASS; save the output as the behavior-preserving refactor baseline.

- [ ] **Step 3: Reuse only the applicable proven ISO-15765 constants**

Replace the three ISO-only executors' private stock seed and applicable payload tables with the existing `denso_iso15765_can_common.h` constants. TCU and petrol use all four; diesel uses the stock seed table, standard index transformation, and encrypt table only because revision `59f4e442` appends its BEEF read payloads raw. Keep petrol EcuTek/RaceRom/Cobb tables local, and leave DensoCAN on its distinct `{7856,CE22,F513,6E86}` payload table. Update the common header's scope comment to name the three Wave 5 consumers. Record that mode switching, kernel exchange helpers, retry/tolerance policy, geometry, address indexing, startup ordering, and logs were compared and deliberately remain family-local.

```cpp
bytes::Bytes stock_seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kDensoIso15765SeedKeyTable,
                                         kDensoIso15765IndexTransformation);
}
```

Run: `bazel test --config=release //src/backend/flash/ecu/...`

Expected: PASS with unchanged byte-exact scripts.

- [ ] **Step 4: Close documentation**

Update the tail design's Wave 5 row from `TransportKind::CanRaw` to `CanRawIso15765` and describe raw bootloader followed by proprietary kernel traffic over ISO-15765. Mark Wave 5 complete in `docs/modularization-plan.md` without overwriting the newer Step 6a/6b status, record 10 remaining legacy families, and ensure all four matrix rows say automated-only/experimental and disclose every accepted correction or preserved hazard. Record that the new `single_window_plan` core was evaluated and intentionally not widened because its kernel-free, no-test-write, one-window contract does not match this wave.

- [ ] **Step 5: Run the complete verification gate**

Run:

```bash
bazel test --config=release //...
bazel build --config=release //:fastecu
prek run --all-files
bazel run //:clang_tidy_report_changed
bazel test --config=release //:portable_closure //:serial_compat_allowlist //:legacy_flash_drain //:backend_no_widgets
rg -n "FlashEcuSubaruDensoSH7058Can|FlashEcuSubaruDensoSH7058CanDiesel|FlashTcuSubaruDensoSH705xCan|FlashEcuSubaruDensoSH705xDensoCan" src scripts
```

Expected: every command passes; the final `rg` has no production/guard hit; `//:legacy_flash_drain` reports 10 remaining families.

- [ ] **Step 6: Review the full diff for scope and behavior evidence**

Run:

```bash
git diff --check origin/master...HEAD
git diff --stat origin/master...HEAD
git log --oneline origin/master..HEAD
```

Confirm the sequence contains mixed contract, desktop adapter, DensoCAN core, DensoCAN route/removal, TCU core, TCU route/removal, petrol family, diesel family, and factoring/closeout commits. These form the design's five review units (DensoCAN, TCU, petrol, diesel, closeout). Confirm no hardware qualification claim and no Wave 6/7 source changes are present.

- [ ] **Step 7: Commit closeout**

```bash
git add src/backend/flash/ecu docs
git commit -m "docs(flash): complete modularization wave 5"
```
