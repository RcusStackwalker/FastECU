# Single-Window Flash Plan Extraction — Design

**Status:** proposed 2026-09-05.

**Goal:** collapse the duplicated plan-validation skeleton shared by ten
`src/backend/flash/ecu/*_plan.cpp` families behind one spec-driven core,
removing roughly 700 duplicated lines without touching the executor cluster.

**Non-goal:** any change to the flash executors. See
[What stays duplicated](#what-stays-duplicated).

## Findings that set the scope

Measured against `master` at `2a8a3492` using the SonarCloud CLI
(`sonar api GET /api/measures/component_tree` for per-file counts,
`/api/duplications/show` per file for the block pairings).

Project-wide duplication is 19,338 lines (18.9%). On the new-code period it is
8,983 lines in 1,027 blocks. Clustering every new-code duplication block by
the files it spans, and excluding `legacy/` paths and the
`dtc_tables.cpp` data table:

| Cluster | New dup lines | Shape |
| --- | --- | --- |
| `src/backend/flash/ecu/*_executor.cpp` | ~4,200 | 150–190 line blocks across four Denso CAN executors plus three further pairs |
| `src/backend/flash/ecu/*_plan.cpp` | ~758 | Identical skeleton; constants-only differences |
| `src/ui/desktop/ecu_operations.cpp`, `definition_file_convert.cpp` | ~250 | Upstream-inherited Qt UI |
| `src/platform/desktop/common/flash/flash_workflow.cpp` | 100 | Six workflow classes repeating outcome bookkeeping |
| `src/platform/desktop/common/transport/` | ~130 | Five adapters whose `read`/`write` guards differ only by a label string |
| `src/backend/definition/{ecuflash,romraider}_parser.cpp` | ~106 | `parse_axis` / `parse_table` skeleton |
| `src/ui/desktop/definition/definition_authoring_dialog.cpp` | 42 | Two functions sharing a dialog → validate → write tail |
| `src/backend/config/{car_model,protocol}_catalog.cpp` | 12 | XML load preamble and `text_or_empty` |

This design takes the second row only. The remaining rows are recorded here so
a later reader can pick them up without re-running the measurement.

### What stays duplicated

The executor cluster is the largest by a wide margin and is deliberately
excluded. `denso_iso15765_can_common.h` records the decision and its reasoning:
the blocks that look alike differ in read timeouts, retry counts, pre-loop read
counts, image base addresses, and in how strictly a bad response is treated —
and that last one is the safety-relevant part of each family. Collapsing them
"would make a future reader believe these are the same protocol when they are
not."

The same boundary is stated as policy in
[the protocol generalization notes](../../protocol-generalization-opportunities.md):
share pure byte algorithms, framing,
validation primitives, and block planning; keep protocol sequence and safety
policy readable within each verified family. Plan validation is declarative
field checking, not protocol sequence, so it falls on the shareable side of
that line. This design does not reopen the executor question.

### The cluster is exactly ten families

The thirteen `*_plan.cpp` files split cleanly. Ten share one skeleton:

`subaru_denso_1n83m_1_5m_can`, `subaru_denso_1n83m_4m_can`,
`subaru_denso_sh72531_can`, `subaru_denso_sh72543_can_diesel`,
`subaru_hitachi_m32r_can`, `subaru_hitachi_m32r_kline`,
`subaru_mitsu_m32r_kline`, `subaru_tcu_cvt_hitachi_m32r_can`,
`subaru_tcu_cvt_mitsu_mh8104_can`, `subaru_tcu_cvt_mitsu_mh8111_can`.

Three do not, and are out of scope: `subaru_denso_mc68hc16y5_02` and
`subaru_denso_sh7055_02` carry kernel images, kernel-upload validation, and a
protocol-dependent wire-parameter table; `mitsu_colt_m32r_can` carries a
four-entry variant table and builds `ConfirmationSpec` entries. Sonar reports
no duplication in any of the three, which corroborates the split rather than
merely asserting it.

Every one of the ten is, in order: constants → `validate_identity` → eight
further ordered checks → a `build_*` that fills `FlashPlanFields`, calls
`validate_and_build`, then re-validates. A `diff` of any two members differs
only in names, addresses, and message text. Three things genuinely vary:

1. **The flash-geometry check.** One-block, three-block, a four-block loop, and
   MH8104/MH8111's block-3-specific checks are four different shapes.
2. **The family-plan wire-parameter check.** Each targets a different
   `std::get_if` alternative with different members.
3. **Constants and display strings.**

Two further variations are describable as data rather than code:

- **Protocol count.** Nine families accept one protocol id;
  `subaru_hitachi_m32r_kline` accepts two (a normal and a recovery name) and
  derives its session mode from which was used.
- **Read versus write window.** Seven families use one region for both. The
  three TCU CVT families declare them separately, and in
  `subaru_tcu_cvt_mitsu_mh8111_can` they genuinely differ — the read window
  `{0x8000, 0x78000}` ends exactly where the write window `{0x80000, 0x100000}`
  begins. The erase region always equals the write window.

## The abstraction

A new portable target `//src/backend/flash/ecu:single_window_plan`. A
*single-window* family is one described entirely by a protocol id, an MCU, a
read window, a write window, and one image size.

```cpp
struct SingleWindowPlanSpec
{
    std::string_view display_name;                // "Subaru Denso SH72531 CAN"
    std::span<const std::string_view> protocols;  // one entry, or two for the K-Line recovery variant
    std::string_view mcu;
    FlashFamily family;
    TransportKind transport;
    MemoryRegion read_region;
    MemoryRegion write_region;                    // also the erase region
    std::uint32_t image_size;
    bool (*geometry_ok)(const flashdev_t&);
    bool (*wire_params_ok)(const FlashPlan&);
};

Status validate_single_window_plan(const SingleWindowPlanSpec& spec, const FlashPlan& plan);

Result<FlashPlan> build_single_window_plan(const SingleWindowPlanSpec& spec, FlashOperation operation,
                                           std::string_view protocol_name, std::string_view mcu_type,
                                           std::optional<bytes::Bytes> image,
                                           FamilyPlan family_plan);
```

Two plain function pointers rather than templates or `std::function`, so the
spec stays a literal type usable as `constexpr`. They cover exactly the two
variation points that resist a data description:

- `geometry_ok` returns `bool`; the core composes
  `"{mcu} flash geometry is invalid"`.
- `wire_params_ok` returns `bool`, false also covering the null-alternative
  case; the core composes `"{display_name} wire parameters are invalid"`.

`build_single_window_plan` ends by calling `validate_single_window_plan` with
the same spec, so no re-validation callback is needed. The `family_plan` value
is a parameter because `subaru_hitachi_m32r_kline` derives its session mode
from the protocol name.

The ten public `build_*` and `validate_*` signatures are unchanged, so no
caller moves and the family headers are untouched.

### A family after the change

```cpp
namespace
{
constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_sh72531_can"}};

// fblocks_SH72531[1], the window legacy read_memory hardcodes over its own
// arguments (lines 828-830) and the 0x34/0x35 setup PDUs spell out literally
// (lines 844-854, 881-891).
constexpr MemoryRegion kMainBlock{0x00008000, 0x00137F00};
constexpr std::uint32_t kImageStart = 0x00000000; // fblocks[0].start
constexpr std::size_t kImageSize = 0x140000;      // fblocks[0..2] summed, and SH72531's own romsize

// Unlike its N83M_1_5MB sibling, SH72531's flash table is self-consistent:
// romsize (1280 KiB) equals its own fblocks sum, so it is checked here.
bool geometry_ok(const flashdev_t& device)
{
    return device.numblocks == 3 && device.romsize == kImageSize &&
           device.fblocks[0].start == kImageStart && device.fblocks[1].start == kMainBlock.start &&
           device.fblocks[1].len == kMainBlock.length;
}

bool wire_params_ok(const FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruDensoSh72531CanPlan>(&plan.family_plan());
    return p != nullptr && p->request_id == 0x7e0 && p->response_id == 0x7e8 && p->bitrate == 500000 &&
           !p->extended_id && p->lead_pad_len == 0x8000 && p->tail_pad_len == 0x100;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Subaru Denso SH72531 CAN",
    .protocols = kProtocols,
    .mcu = "SH72531",
    .family = FlashFamily::SubaruDensoSh72531Can,
    .transport = TransportKind::CanIso15765,
    .read_region = kMainBlock,
    .write_region = kMainBlock,
    .image_size = kImageSize,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};
} // namespace

Status validate_subaru_denso_sh72531_can_plan(const FlashPlan& plan)
{
    return validate_single_window_plan(kSpec, plan);
}

Result<FlashPlan> build_subaru_denso_sh72531_can_plan(FlashOperation operation, std::string_view protocol_name,
                                                      std::string_view mcu_type, std::optional<bytes::Bytes> image)
{
    return build_single_window_plan(kSpec, operation, protocol_name, mcu_type, std::move(image),
                                    SubaruDensoSh72531CanPlan{0x7e0, 0x7e8, 500000, false, 0x8000, 0x100});
}
```

Each family file lands around 45–55 lines, down from 134–166. The provenance
comments that cite legacy line numbers stay attached to the constants and
predicates they explain — that is the property worth protecting, and it
survives.

## Error messages

Message text is **not** preserved byte-for-byte. Generalization normalizes it,
and that is an accepted outcome of this change rather than a regression.

The current messages carry two names per family — a qualified form in
`"Unsupported {} protocol: {}"` and `"plan is not for {}"`, and a shorter form
in the wire, transfer-region, kernel-free, and erase-region messages. These
collapse to the single qualified `display_name`, so for example
`"Denso SH72531 CAN transfer region is invalid"` becomes
`"Subaru Denso SH72531 CAN transfer region is invalid"`.

One pre-existing defect fixes itself. `subaru_tcu_cvt_hitachi_m32r_can` uses
the short name `"Hitachi M32R CAN"`, identical to the unrelated non-TCU
`subaru_hitachi_m32r_can` family, so today those two families emit
indistinguishable errors. Under one qualified name they become
`"Subaru TCU CVT Hitachi M32R CAN"` and `"Subaru Hitachi M32R CAN"`.

The geometry message loses its block-count descriptor: `"SH72531 three-block
flash geometry is invalid"` becomes `"SH72531 flash geometry is invalid"`. The
block count is already implied by the MCU names the table uses
(`M32R_512KB_4blocks`, `M32R_512KB_1block`).

The image-size clause keeps `0x{:X}` — uppercase, matching how hex constants
are written elsewhere in this package, and incidentally leaving the nine
existing test assertions untouched. The "got" clause stays lowercase `0x{:x}`,
as today.

## Testing

The ten existing `*_plan_test.cpp` suites are the regression net and are not
rewritten. They pin behaviour — which check fires, in which order, with which
`ErrorKind` — which is the property that matters. Across all ten they contain
exactly nine message-text assertions, every one on the image-size hex string,
so message normalization does not disturb them.

A new `single_window_plan_test.cpp` (`fastecu_portable_gtest`) drives the core
directly against a synthetic spec, covering each check's failure path and the
message it composes. This is where the shared logic earns its own coverage
rather than being tested only through ten indirect callers.

## Build graph

- New `cc_library(name = "single_window_plan", ...)` in
  `src/backend/flash/ecu/BUILD.bazel`, with the same `deps` the plan targets
  already carry (`//src/backend/definitions:models`,
  `//src/backend/flash:flash_device_lookup`, `//src/backend/flash:flash_plan`,
  `//src/backend/flash:flash_validation`, `//src/backend/ports`).
- A `":single_window_plan"` dep added to each of the ten plan targets.
- `fastecu_portable_gtest(name = "single_window_plan_test", ...)`.
- `"single_window_plan"` added to `PORTABLE_ROOTS[ROOT / "src/backend/flash/ecu"]`
  in `scripts/check-portable-closure.py`. That set is a must-exist-and-be-Qt-free
  list, so a new portable target in the package must be named there.
- No `genquery` edit. The new library is reached transitively through the ten
  plan targets already listed in `portable_backend_closure`.

## Documentation

[the protocol generalization boundary](../../protocol-generalization-opportunities.md)
gains an entry under "Consolidated foundations" recording that single-window
plan validation is now shared, and restating that the executor cluster
deliberately is not. Without that line the next reader re-derives this whole
analysis from scratch.

## Sequencing

1. Land `single_window_plan.h/.cpp` and its test, and convert
   `subaru_denso_sh72531_can` — the plainest member, one protocol, one region.
   Confirm its existing suite passes unchanged.
2. Convert the remaining six single-region families.
3. Convert the three TCU CVT families last, as the ones with distinct read and
   write windows.

Each step keeps `bazel test --config=release //src/backend/flash/ecu:all`
green.

## Expected result

Roughly 700 duplicated lines removed from the new-code period. Ten family files
drop from 134–166 lines to 45–55. `src/backend/flash/ecu/` duplication falls
from 4,889 new duplicated lines against 9,582 ncloc; the executor share of that
figure is unchanged and remains deliberate.
