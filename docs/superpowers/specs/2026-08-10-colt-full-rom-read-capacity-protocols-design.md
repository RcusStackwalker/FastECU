# Colt Full-ROM Reads and Capacity-Specific Protocols

## Goal

Make Mitsubishi Colt CAN ROM files address-aligned by reading the bootloader
region instead of omitting it. Separate 384 KiB and 512 KiB behavior into two
normal/vendor-authorization protocol pairs, and require writes to match the
selected capacity.

## Protocols

The existing protocol pair remains the 384 KiB option:

- `mitsu_ecu_m32r_can`
- `mitsu_ecu_m32r_can_vendor_ext`

Add a 512 KiB pair:

- `mitsu_ecu_m32r_can_512kb`
- `mitsu_ecu_m32r_can_vendor_ext_512kb`

The `vendor_ext` component controls only the preliminary vendor authorization.
The `_512kb` component controls only ROM capacity. Desktop dispatch must derive
these properties independently so the combined vendor/512 variant enables both.

All four protocols remain available for reading and writing. Their configuration
descriptions and vehicle-list entries must make the capacity and authorization
variant clear.

## Read Behavior

The existing pair reads `0x00000` through `0x5ffff` and returns exactly
`0x60000` bytes (384 KiB). The new pair reads `0x00000` through `0x7ffff` and
returns exactly `0x80000` bytes (512 KiB).

Both modes therefore include the protected `0x00000`-`0x07fff` bootloader in
the output. File offsets equal ECU addresses, eliminating the previous 32 KiB
misalignment. Read progress covers the entire selected capacity and the first
wire request targets address zero.

The plan builder selects the read extent explicitly from the Colt protocol ID
rather than depending on the first block of generic MCU metadata. Unknown Colt
protocol IDs fail validation instead of silently choosing a capacity.

## Write Behavior

The existing pair accepts only an exact `0x60000`-byte image. It never erases or
writes the bootloader prefix. It compares, writes, and verifies only
`0x08000`-`0x60000`. It does not use the top-region redirect helpers and does
not request the top-128 KiB bootstrap confirmation.

The new pair accepts only an exact `0x80000`-byte image. It likewise preserves
the bootloader prefix, while comparing, writing, and verifying
`0x08000`-`0x80000`. It retains the conditional `0x60000`-`0x80000` bootstrap
flow, redirect helpers, and top-region confirmation introduced for full-flash
writes.

Both capacities retain the erase-trigger confirmation. Image slices always use
absolute ECU addresses as file offsets, so the bytes written at address `A`
come from file offset `A`.

An image of the other capacity is rejected during plan construction, before
any operator confirmation, worker startup, transport access, erase, or write.

## Components

- `protocols.cfg` registers both new protocols and vehicle choices and assigns
  capacity-appropriate MCU metadata.
- Main-window dispatch recognizes all four IDs and independently determines
  vendor authorization and 512 KiB capacity.
- `MitsuColtM32rCanPlan` carries the selected physical ROM capacity so executor
  behavior does not need to parse protocol strings.
- The Colt plan builder validates the exact supported IDs, constructs the
  capacity-specific read/write region, validates image size, and declares only
  the confirmations used by that capacity.
- The executor reads the plan's complete read region. Its write path runs the
  top-region comparison/bootstrap only for a 512 KiB plan and limits normal
  write/read-back verification to the selected ROM end.
- The dialog shows the top-region warning only when the built plan requires its
  confirmation. Existing confirmation collection remains pre-execution.

## Error Handling

Unsupported Colt protocol IDs and capacity mismatches return `InvalidConfig`
with the expected size in the detail. Existing typed transport, timeout,
cancellation, and bad-response behavior remains unchanged. No partial data is
reported as a successful read.

## Testing

Tests are written before production changes and demonstrate these behaviors:

- plans map each of the four protocol IDs to the correct capacity and vendor
  flag;
- an unsupported near-match is rejected;
- reads begin at zero and produce exactly 384 KiB or 512 KiB;
- each write pair accepts only its exact image size;
- 384 KiB writes cover `0x08000`-`0x60000`, preserve the bootloader, and omit
  top-region bootstrap work and confirmation;
- 512 KiB writes cover `0x08000`-`0x80000`, preserve the bootloader, and retain
  conditional top-region bootstrap behavior;
- desktop routing enables vendor authorization for both vendor variants and
  capacity selection for both 512 KiB variants;
- dialog prompts match the confirmations declared by each plan;
- protocol configuration exposes all four choices with correct capabilities.

Focused Bazel tests for the Colt plan, executor, desktop dialog/routing, and
configuration are followed by the relevant aggregate test suite and a clean
build target.

## Non-Goals

This change does not make the bootloader writable, alter the diagnostic or
security algorithms, add `test_write`, generalize capacity selection to other
ECU families, or distribute ROM contents.
