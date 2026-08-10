# Colt Full-ROM Reads and Capacity-Specific Protocols Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Colt ROM reads address-aligned and provide distinct 384 KiB and 512 KiB normal/vendor protocol pairs with matching write-size and write-range behavior.

**Architecture:** Parse the four supported protocol IDs once into a capacity/vendor descriptor and snapshot those values in `MitsuColtM32rCanPlan`. The plan builder owns exact image-size and region validation; the executor consumes only the typed plan, while the desktop dialog derives its prompts from plan confirmations rather than string tests.

**Tech Stack:** C++23, Qt 6, GoogleTest/GoogleMock, Bazel, XML-like `protocols.cfg` configuration.

## Global Constraints

- Existing protocols read and write exactly `0x60000` bytes; `_512kb` protocols read and write exactly `0x80000` bytes.
- Every successful read starts at `0x00000`, so file offsets equal ECU addresses.
- Writes never erase or write `0x00000`-`0x07fff`.
- Only 512 KiB writes use the `0x60000`-`0x80000` redirect/bootstrap flow and `TopRegionBootstrap` confirmation.
- All writes retain `EraseTrigger`; `test_write` remains unsupported.
- Normal and vendor authorization are identical apart from the vendor challenge handshake.
- Do not add real ROM bytes or broaden behavior to other ECU families.

---

### Task 1: Protocol classification and capacity-specific plans

**Files:**
- Modify: `src/backend/flash/flash_types.h`
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h`
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_plan.cpp`
- Test: `src/backend/flash/ecu/mitsu_colt_m32r_can_plan_test.cpp`

**Interfaces:**
- Produces: `MitsuColtProtocolOptions { bool use_vendor_challenge; std::uint32_t rom_size; }`.
- Produces: `Result<MitsuColtProtocolOptions> parse_mitsu_colt_protocol(std::string_view)` accepting exactly the four IDs in the design.
- Produces: `MitsuColtM32rCanPlan::rom_size`, snapshotted as `0x60000` or `0x80000`.
- Changes: `build_mitsu_colt_m32r_can_plan(FlashOperation, std::string_view, std::string_view, std::optional<bytes::Bytes>)` removes the caller-supplied vendor boolean and derives both properties from `protocol_name`.

- [ ] **Step 1: Read the test-quality rules before editing tests**

Read `superpowers/test-driven-development/writing-good-tests.md` from the installed Superpowers skill directory. Name the production behavior that makes each new assertion fail: protocol parsing, read start/length, write size, write range, or confirmation set.

- [ ] **Step 2: Write failing classification and read-plan tests**

Add table-driven assertions equivalent to:

```cpp
struct Case { std::string_view id; bool vendor; std::uint32_t size; };
for (const Case test : std::to_array<Case>({
         {"mitsu_ecu_m32r_can", false, 0x60000},
         {"mitsu_ecu_m32r_can_vendor_ext", true, 0x60000},
         {"mitsu_ecu_m32r_can_512kb", false, 0x80000},
         {"mitsu_ecu_m32r_can_vendor_ext_512kb", true, 0x80000},
     })) {
    const auto plan = build_mitsu_colt_m32r_can_plan(
        FlashOperation::Read, test.id, test.size == 0x80000 ? "M32R_512KB_1block"
                                                            : "M32R_384KB_1block",
        std::nullopt);
    ASSERT_TRUE(plan.has_value()) << test.id;
    EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, test.size}));
    const auto& family = std::get<MitsuColtM32rCanPlan>(plan->family_plan());
    EXPECT_EQ(family.use_vendor_challenge, test.vendor);
    EXPECT_EQ(family.rom_size, test.size);
}
```

Add a rejection test for `mitsu_ecu_m32r_can_vendor_ext_512kb_typo` expecting `InvalidConfig`.

- [ ] **Step 3: Run the plan test and verify RED**

Run:

```bash
bazel test //src/backend/flash/ecu:mitsu_colt_m32r_can_plan_test
```

Expected: compilation or assertion failure because the parser/`rom_size` interface and zero-based read regions do not exist.

- [ ] **Step 4: Add failing write-size, write-region, and confirmation tests**

Cover both capacities with exact-size images and cross-capacity rejection:

```cpp
EXPECT_EQ(plan384->transfer_region(), (MemoryRegion{0x8000, 0x58000}));
EXPECT_THAT(plan384->confirmations(),
            ElementsAre(Field(&ConfirmationSpec::id,
                              ConfirmationSpec::Id::EraseTrigger)));

EXPECT_EQ(plan512->transfer_region(), (MemoryRegion{0x8000, 0x78000}));
EXPECT_THAT(plan512->confirmations(), UnorderedElementsAre(
    Field(&ConfirmationSpec::id, ConfirmationSpec::Id::EraseTrigger),
    Field(&ConfirmationSpec::id, ConfirmationSpec::Id::TopRegionBootstrap)));
```

Assert a 512 KiB image is rejected for the default ID, a 384 KiB image is rejected for `_512kb`, and each error detail contains `0x60000` or `0x80000` respectively.

- [ ] **Step 5: Implement minimal typed classification and plan construction**

Implement exact-ID classification without prefix fallthrough:

```cpp
struct MitsuColtProtocolOptions {
    bool use_vendor_challenge;
    std::uint32_t rom_size;
};

Result<MitsuColtProtocolOptions> parse_mitsu_colt_protocol(std::string_view protocol_name);
```

For reads, set `MemoryRegion{0, options.rom_size}`. For writes, require `image->size() == options.rom_size`, set `MemoryRegion{kUserspaceStart, options.rom_size - kUserspaceStart}`, always add `EraseTrigger`, and add `TopRegionBootstrap` only when `rom_size == kFullRomSize`. Keep the MCU lookup validation and bootload session behavior.

- [ ] **Step 6: Run plan tests and verify GREEN**

Run the Task 1 Bazel target and confirm zero failures.

- [ ] **Step 7: Commit Task 1**

```bash
git add src/backend/flash/flash_types.h src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h src/backend/flash/ecu/mitsu_colt_m32r_can_plan.cpp src/backend/flash/ecu/mitsu_colt_m32r_can_plan_test.cpp
git commit -m "feat: add capacity-specific Colt flash plans"
```

---

### Task 2: Execute zero-based reads and capacity-bounded writes

**Files:**
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`
- Test: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`

**Interfaces:**
- Consumes: `MitsuColtM32rCanPlan::rom_size` from Task 1.
- Preserves: `read_flash_range(Ctx&, const MitsuColtM32rCanPlan&, std::uint32_t, std::uint32_t, bool)` and existing transport protocol helpers.
- Produces: read result size equal to the plan capacity; write logic bounded by `rom_size`.

- [ ] **Step 1: Write failing read-request tests**

Refactor the scripted-read helper only as needed to avoid duplicating 6,144/8,192 chunk expectations. Add one test per capacity that scripts data from address zero, executes the real executor, and asserts:

```cpp
ASSERT_TRUE(result.has_value());
ASSERT_TRUE(result->read_bytes.has_value());
EXPECT_EQ(result->read_bytes->size(), expected_size);
EXPECT_EQ(result->read_bytes->front(), marker_for_address_zero);
EXPECT_EQ(result->read_bytes->back(), marker_for_last_address);
EXPECT_THAT(events.progress_events,
            Contains(std::pair<int, int>{static_cast<int>(expected_size),
                                         static_cast<int>(expected_size)}));
```

The first scripted request must be `buildReadMemoryByAddress(0, kFlashReadBlockSize)`.

- [ ] **Step 2: Run executor tests and verify read RED**

Run:

```bash
bazel test //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test
```

Expected: failure because current default reads start at `0x8000` and stop at `0x60000`.

- [ ] **Step 3: Implement minimal read support**

Keep the executor's generic call to `read_flash_range`, relying on Task 1's zero-based `transfer_region`. Update stale legacy comments and test helpers; do not special-case capacity in the read loop.

- [ ] **Step 4: Add failing 384 KiB write-boundary tests**

Build a default-protocol plan with a synthetic `0x60000` image. Script the normal userspace helpers and write/verify exchanges through `0x5ffff`. Assert no read request, upload, redirect-helper request, or progress total references `0x60000`-`0x80000`, and successful progress ends at `0x58000` bytes.

Add a guard test constructing an otherwise-valid 384 KiB family plan whose image is 512 KiB; executor validation must reject it before transport I/O.

- [ ] **Step 5: Run executor tests and verify write RED**

Run the Task 2 target. Expected: the current executor requires `kFullRomSize`, reads the top region, or attempts bootstrap work for the default protocol.

- [ ] **Step 6: Implement capacity-bounded write execution**

Replace unconditional full-ROM assumptions with `family.rom_size`:

```cpp
const bool includes_top_region = family.rom_size == MitsuColtCan::kFullRomSize;
const std::uint32_t writable_end = family.rom_size;
```

Validate `image.size() == family.rom_size`. Run top-region comparison/bootstrap only when `includes_top_region`; for 384 KiB plans select the stock page helpers directly. Slice upload and verification data using absolute offsets `[kUserspaceStart, writable_end)`, and use `writable_end - kUserspaceStart` as aggregate progress total. Retain existing 512 KiB bootstrap tests unchanged in substance.

- [ ] **Step 7: Run executor tests and verify GREEN**

Run the Task 2 target and confirm both capacity suites and all failure-path tests pass.

- [ ] **Step 8: Commit Task 2**

```bash
git add src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp
git commit -m "feat: bound Colt writes to selected ROM capacity"
```

---

### Task 3: Route all variants and show plan-driven confirmations

**Files:**
- Modify: `src/ui/desktop/mainwindow.cpp`
- Modify: `src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h`
- Modify: `src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.cpp`
- Test: `src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can_dialog_test.cpp`

**Interfaces:**
- Consumes: Task 1's protocol-derived plan; callers no longer pass a vendor boolean.
- Produces: `FlashEcuMitsuM32rCan(SerialPortActions*, FileActions::EcuCalDefStructure*, const QString&, QWidget*)` with authorization and capacity derived by `buildPlan()` from `ecuCalDef->FlashMethod`.
- Consumes: `FlashPlan::confirmations()` to decide which write prompts are displayed.

- [ ] **Step 1: Write failing dialog plan and prompt tests**

Update construction calls to remove the boolean. Add tests proving `buildPlan()` maps `mitsu_ecu_m32r_can_vendor_ext_512kb` to `use_vendor_challenge == true` and `rom_size == 0x80000`.

Use a `0x60000` default image and assert prompt titles are exactly:

```cpp
QStringList{"Connecting to ECU", "Erase trigger"}
```

Use a `0x80000` `_512kb` image and assert prompt titles are:

```cpp
QStringList{"Connecting to ECU", "Erase trigger", "Top 128KB bootstrap"}
```

Also assert the erase warning says `384 KiB` for the default protocol and `512 KiB` for the new protocol.

- [ ] **Step 2: Run dialog tests and verify RED**

Run:

```bash
bazel test //src/ui/desktop/flash/ecu:test_flash_ecu_mitsu_m32r_can_dialog
```

Expected: failures because the constructor still accepts a vendor flag, default writes require `0x80000`, and the top-region prompt is unconditional.

- [ ] **Step 3: Implement plan-driven dialog behavior and routing**

Remove `useVendorChallenge` from the dialog constructor/member and from MainWindow construction. Pass only the protocol ID already stored in `ecuCalDef->FlashMethod` to the Task 1 builder.

After successful preflight, use a helper equivalent to:

```cpp
const auto requires = [&plan](ConfirmationSpec::Id id) {
    return std::ranges::any_of(plan.confirmations(),
        [id](const ConfirmationSpec& spec) { return spec.id == id; });
};
```

Show each warning only when its confirmation is present. Derive displayed capacity from `std::get<MitsuColtM32rCanPlan>(plan.family_plan()).rom_size`. Move the plan into the worker only after all required prompts succeed. Keep `startsWith("mitsu_ecu_m32r_can")` dispatch so all exact IDs reach this dialog; exact-ID validation remains in the backend parser.

- [ ] **Step 4: Run dialog tests and verify GREEN**

Run the Task 3 target and confirm zero failures.

- [ ] **Step 5: Commit Task 3**

```bash
git add src/ui/desktop/mainwindow.cpp src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.cpp src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can_dialog_test.cpp
git commit -m "feat: route Colt capacity protocol variants"
```

---

### Task 4: Register protocols, update qualification docs, and verify the feature

**Files:**
- Modify: `resources/shared/config/protocols.cfg`
- Modify or create the focused config test under: `src/backend/config/`
- Modify: `docs/flash-qualification-matrix.md`
- Modify: `docs/colt_czt_47110032_can_bench_checklist.md`

**Interfaces:**
- Consumes: the exact four IDs parsed in Task 1.
- Produces: four selectable car/protocol entries with `read=yes`, `write=yes`, `test_write=no`, ISO-15765 transport, and capacity-matching MCU names.

- [ ] **Step 1: Write a failing configuration test**

Extend the existing protocols configuration parser test (or add a focused `cc_test` beside it if no current assertion surface fits) to load `PROTOCOLS_CFG_PATH` and assert:

```cpp
// Existing pair
EXPECT_EQ(protocol("mitsu_ecu_m32r_can").mcu, "M32R_384KB_1block");
EXPECT_EQ(protocol("mitsu_ecu_m32r_can_vendor_ext").mcu, "M32R_384KB_1block");
// New pair
EXPECT_EQ(protocol("mitsu_ecu_m32r_can_512kb").mcu, "M32R_512KB_1block");
EXPECT_EQ(protocol("mitsu_ecu_m32r_can_vendor_ext_512kb").mcu,
          "M32R_512KB_1block");
```

For all four, assert read/write enabled and test-write disabled. Assert each ID appears in a Mitsubishi Colt car-model entry.

- [ ] **Step 2: Run the focused config test and verify RED**

Use `bazel query 'kind(".*test", //src/backend/config:*)'` to identify the existing parser test target, then run that exact target. Expected: missing `_512kb` protocols and car entries.

- [ ] **Step 3: Add both protocol and vehicle entries**

Copy the existing normal/vendor fields, changing only name, MCU, and description as follows:

```xml
<protocol name="mitsu_ecu_m32r_can_512kb">
    <ecu>Mitsubishi M32R</ecu>
    <mcu>M32R_512KB_1block</mcu>
    <mode>OBD2</mode>
    <checksum>no</checksum>
    <read>yes</read>
    <test_write>no</test_write>
    <write>yes</write>
    <flash_transport>iso15765</flash_transport>
    <log_transport>CAN</log_transport>
    <log_protocol>CDBG</log_protocol>
    <cal_id_ascii>no</cal_id_ascii>
    <cal_id_addr>0x0</cal_id_addr>
    <cal_id_length>0</cal_id_length>
    <description>Mitsubishi Colt CZT Z37A CAN full flash (M32176F3/512KB, ROM 47110032)</description>
</protocol>
<protocol name="mitsu_ecu_m32r_can_vendor_ext_512kb">
    <ecu>Mitsubishi M32R</ecu>
    <mcu>M32R_512KB_1block</mcu>
    <mode>OBD2</mode>
    <checksum>no</checksum>
    <read>yes</read>
    <test_write>no</test_write>
    <write>yes</write>
    <flash_transport>iso15765</flash_transport>
    <log_transport>CAN</log_transport>
    <log_protocol>CDBG</log_protocol>
    <cal_id_ascii>no</cal_id_ascii>
    <cal_id_addr>0x0</cal_id_addr>
    <cal_id_length>0</cal_id_length>
    <description>Mitsubishi Colt CZT Z37A CAN vendor diagnostic extension full flash (M32176F3/512KB, ROM 47110032)</description>
</protocol>
```

Add matching car-model entries whose versions explicitly say `512KB` and, for the vendor entry, `vendor diagnostic extension`.

- [ ] **Step 4: Update operational documentation**

In the qualification matrix, state that 384 KiB protocols read `0x00000`-`0x60000` and write `0x08000`-`0x60000`, while `_512kb` protocols read `0x00000`-`0x80000` and write `0x08000`-`0x80000` with conditional bootstrap. Update the bench checklist to require separate read-size/address-alignment checks and write qualification for both capacities.

- [ ] **Step 5: Run focused and aggregate verification**

Run fresh commands:

```bash
bazel test //src/backend/flash/ecu:mitsu_colt_m32r_can_plan_test
bazel test //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test
bazel test //src/ui/desktop/flash/ecu:test_flash_ecu_mitsu_m32r_can_dialog
bazel test //src/backend/config:all
bazel build //:fastecu
git diff --check
```

If `//src/backend/config:all` or `//:fastecu` is not a valid target, use `bazel query` to select the corresponding complete config test set or application target and record the exact substituted command. Every command must exit zero before reporting completion.

- [ ] **Step 6: Review requirements against the approved spec**

Confirm from the diff and tests: four exact IDs; zero-based 384/512 reads; exact capacity-specific write validation; protected bootloader; no default top bootstrap; retained 512 bootstrap; correct vendor handshake for both vendor IDs; correct prompts and configuration entries; no real ROM content.

- [ ] **Step 7: Commit Task 4**

```bash
git add resources/shared/config/protocols.cfg src/backend/config docs/flash-qualification-matrix.md docs/colt_czt_47110032_can_bench_checklist.md
git commit -m "docs: register Colt full-flash protocols"
```
