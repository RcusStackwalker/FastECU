# Step 5 Tail Wave 6c-2 — Hitachi M32R JTAG Removal — Design

**Status:** implemented on this branch; implementation plan in [the 6c-2 plan](../plans/2026-09-25-step5-tail-wave6c2-hitachi-m32r-jtag-removal.md).
**Parent:** [wave 6 singletons](2026-09-19-step5-tail-wave6-singletons-design.md).
**Predecessor:** 6c-1 Denso MC68HC16Y5 BDM (#357), merged.
**Source baseline:** `766475b8`. That commit is the last to contain the
legacy JTAG source; `git show 766475b8:<path>` recovers it.

## Intent and success criteria

Remove `FlashEcuSubaruHitachiM32rJtag` instead of migrating it, reducing
`//:legacy_flash_drain` from three entries to two. No portable code is added.

Done means:

- `jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp` is out of
  `REMAINING`; the legacy operation, its dialog package
  (`src/ui/desktop/flash/jtag/`), and the `MainWindow` JTAG dispatch branch
  are deleted.
- `//src/ui/desktop/flash/jtag:__pkg__` is gone from the `serial_qt_compat`
  visibility list and from `FROZEN` in
  `scripts/check-serial-compat-allowlist.py`.
- The matrix row stays and records the removal.
- The probe's wire sequence survives in the appendix below.

## Findings that shape the design

### The family is unreachable

No `<protocol>` entry in `resources/shared/config/protocols.cfg` produces a
`sub_ecu_hitachi_m32r_jtag*` name, so `MainWindow`'s `startsWith` branch can
never fire. The matrix and the
[tail design](2026-08-08-step5-tail-flash-drain-design.md) already record
this.

### Read and Write are empty stubs that report success

`read_mem()` and `write_mem()` have empty bodies returning `STATUS_SUCCESS`.
`execute()` runs a probe, then calls one of them, so Read returns success with
no ROM, and Write and TestWrite tell the operator the operation succeeded
without writing a byte.

### The only real I/O is a probe that cannot fail

`init_jtag()` runs hard reset, IDCODE, USERCODE and the tool-ROM sequence (see
the appendix) and ignores every return value, so a dead or absent adapter
still yields success. The ASCII TAP-shift helpers (`write_jtag_ir`,
`write_jtag_dr`, `read_jtag_dr`, `read_response`) are reachable only from
`set_rtdenb()`, whose one call is commented out.

## Decision

**Remove, do not migrate.** Taken with the user during brainstorming; it
overrides the parent spec's 6c-2 row and the tail design's "ported as-is
without wiring a new dispatch path".

Migrating would mean either porting the stubs, which legitimizes a no-op as
success, or porting the probe and returning `Unsupported` for every
operation, which is new portable code that is still unreachable. The 5c EEPROM
precedent rejects the first. The second preserves knowledge, which this
spec's appendix does without code, tests or a `FlashFamily` value. Wiring a
route would add a user-visible capability, which is out of scope for a drain
PR.

## Changes

### Deleted

- `src/platform/desktop/common/flash/legacy/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.{h,cpp}`.
- `src/ui/desktop/flash/jtag/` — dialog, header and `BUILD.bazel`.
- `mainwindow.cpp`: the `sub_ecu_hitachi_m32r_jtag` branch and its
  "Hitachi ECU JTAG" comment banner. `mainwindow.h`: the JTAG include.
  `src/ui/desktop/BUILD.bazel`: the `//src/ui/desktop/flash/jtag` dep.
- `src/backend/definitions/kernelcomms.h`: the "JTAG commands" block, from
  its banner through `SUB_KERNEL_JTAG_IR_ACK` — 25 macros whose only user is
  the deleted operation. `SUB_KERNEL_BLANK_PAGE`, just above it, is unrelated
  and stays.

### Edited

- `src/platform/desktop/common/flash/legacy/BUILD.bazel`: the jtag entries
  leave `srcs` and `hdrs`, which are explicit lists rather than the globs the
  parent spec describes. The comment records that wave 6c-2 removed the
  `jtag/` subdirectory and that two family subdirectories remain.
- `scripts/check-legacy-flash-drain.py`: the `REMAINING` entry is removed.
- `src/platform/desktop/common/serial/BUILD.bazel` and
  `scripts/check-serial-compat-allowlist.py`: the
  `//src/ui/desktop/flash/jtag:__pkg__` entry is removed from both. Both are
  ratchets, so removal is permitted and nothing is added.

### Behavior after removal

A `sub_ecu_hitachi_m32r_jtag*` protocol name, if one could be produced, would
fall through to `MainWindow`'s existing "Unknown flashmethod … not yet
implemented" warning. The operator-visible change is from a false success to
an honest refusal, on a path no configuration can reach.

## Documentation

- **Matrix row `FlashEcuSubaruHitachiM32rJtag`** stays, per the rule that no
  family silently disappears: `operations` = `none — removed in wave 6c-2`,
  `portable`, `automated_evidence` and `hardware_evidence` = `—`,
  `hardware_status` = `unqualified`, and `notes` link to this spec and state
  why.
- **Tail design:** a note beside "ported as-is without wiring a new dispatch
  path" says it was superseded by removal in 6c-2.
- **Wave-6 singletons design:** a 6c-2 implementation note; the drain goal
  reads ten entries to one, one of them by removal.
- **Modularization plan:** 6c-1 becomes merged (#357); a 6c-2 line records
  the removal and the drain at two remaining families.

## Verification

No new tests: nothing portable is added, and the guards carry the change.

- `bazel build --config=release //:fastecu`.
- `bazel test --config=release //...` — includes `//:legacy_flash_drain`,
  `//:serial_compat_allowlist` and `//:portable_closure`.
- `prek run --all-files` — includes the lychee link check on the new links.
- `git grep -n -i "M32rJtag\|m32r_jtag\|flash/jtag"` returns only
  documentation hits.

## Delivery

One PR from `feat/wave6c2-hitachi-m32r-jtag-removal`. Commits: this spec, the
code removal with guard updates, then the documentation amendments.

## Out of scope

- Unisia Jecs M32R (6c-3) and bootmode (wave 7).
- Any working M32R JTAG read or write.
- Other unused macros in `kernelcomms.h`.

## Appendix: the legacy probe wire sequence

Recorded from `flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp` at
`766475b8`, for anyone reviving M32R JTAG. It is a description of legacy
behavior, not a validated protocol: no hardware evidence exists for it.

### Session

`set_add_iso14230_header(false)`; ISO-14230, CAN, ISO-15765 and 29-bit flags
all false; baud `4800`; no parity or framing set explicitly. `tester_id =
0xF0` and `target_id = 0x10` are assigned and never used. The same probe runs
for read, test_write and write.

### Framing

Each request is `BE EF 00 <len> <payload…> <sum8>`, where `<len>` is the
payload length and `<sum8>` is the 8-bit sum of every preceding byte,
header included. Requests use `write_serial_data()` (no echo drain; see the
parent spec). Each request is followed by a 10 ms delay and one
`read_serial_data(200 ms)`, the framed K-Line reader, which accepts the
`BE EF` header on direct serial.

A reply passes when it is longer than four bytes and `[0] = BE`, `[1] = EF`,
`[4] = <command byte> + 0x40` and `[8] = 0x31` (`SUB_KERNEL_JTAG_IR_ACK`).
The gates check only `length > 4` and then read `[8]`, out of bounds on a
5–8-byte reply. IDCODE and USERCODE additionally take `mid(9, 4)`, which
shortens silently rather than failing, and then read `response.at(0..3)`,
out of bounds on a reply shorter than 13 bytes.

### Sequence

| Step | Payload | Expected `[4]` | Notes |
|---|---|---|---|
| Hard reset | `80` | — | Reply read and ignored. |
| IDCODE | `01` | `41` | `[9..12]` big-endian: version bits 31–28, part number 27–12, manufacturer ID 11–1. |
| USERCODE | `30` | `70` | `[9..12]` big-endian: ROM bits 11–8, ISA 7–4, SDI 3–0. |
| BSR sample | `40 01 02 20 00 00 01 D7` | `80` | 471-bit (`0x1D7`) boundary scan, IR `SAMPLE`, sub-command `READ_BSR`. Replies are read and gated repeatedly until a read returns nothing. |
| MON_CODE ×12 | `40 10 01 20 w0 w1 w2 w3` | `80` | Outer loop 4, inner loop 3: words 0, 1, 2 of the tool-ROM code, four times over. |
| MON_CODE | `40 10 01 20 7F F4 F0 00` | `80` | Word 3. |
| MON_ACCESS | `40 13 01 04 00 00 00 01` | `80` | |
| MON_ACCESS | `40 13 01 04 00 00 00 00` | `80` | |
| MON_DATA ×5 | `40 11 00 20` | `80` | 100 ms delay after each. |

Tool-ROM code (`inst_tool_rom_code`), as four big-endian words:

```text
D1 C0 FF 00   A0 C1 3F FC   20 44 F0 00   7F F4 F0 00
```

The payload bytes are `SUB_KERNEL_JTAG_COMMAND` (`0x40`), an IR or an SDI
register selector (`IR_SAMPLE 0x01` is an IR; `MON_CODE 0x10`, `MON_DATA
0x11` and `MON_ACCESS 0x13` are register selectors — see the register map
below), a sub-command (`READ 0x00`, `WRITE 0x01`, `READ_BSR 0x02`), and a
bit count.

### Register map (from the deleted kernelcomms.h)

Copied from the "JTAG commands" block of `kernelcomms.h` at `766475b8`.
"Used" marks the macros the live probe (the code path that actually ran,
not the commented-out branches or `set_rtdenb()`) sent or checked.

| Group | Macro | Value | Used by live probe |
|---|---|---|---|
| Commands | `SUB_KERNEL_READ_USERCODE` | `0x30` | yes |
| Commands | `SUB_KERNEL_JTAG_COMMAND` | `0x40` | yes |
| Instruction registers | `SUB_KERNEL_IR_EXTEST` | `0x00` | no (commented out) |
| Instruction registers | `SUB_KERNEL_IR_SAMPLE` | `0x01` | yes |
| Instruction registers | `SUB_KERNEL_IR_IDCODE` | `0x02` | no |
| Instruction registers | `SUB_KERNEL_IR_BYPASS` | `0x3F` | no |
| Registers | `SUB_KERNEL_IDCODE` | `0x02` | no |
| Registers | `SUB_KERNEL_USERCODE` | `0x03` | no |
| Registers | `SUB_KERNEL_MDM_SYSTEM` | `0x08` | no |
| Registers | `SUB_KERNEL_MDM_CONTROL` | `0x09` | no |
| Registers | `SUB_KERNEL_MDM_SETUP` | `0x0A` | no |
| Registers | `SUB_KERNEL_MTM_CONTROL` | `0x0F` | no |
| Registers | `SUB_KERNEL_MON_CODE` | `0x10` | yes |
| Registers | `SUB_KERNEL_MON_DATA` | `0x11` | yes |
| Registers | `SUB_KERNEL_MON_PARAM` | `0x12` | no |
| Registers | `SUB_KERNEL_MON_ACCESS` | `0x13` | yes |
| Registers | `SUB_KERNEL_DMA_RADDR` | `0x18` | no |
| Registers | `SUB_KERNEL_DMA_RDATA` | `0x19` | no |
| Registers | `SUB_KERNEL_DMA_RTYPE` | `0x1A` | no |
| Registers | `SUB_KERNEL_DMA_ACCESS` | `0x1B` | no |
| Registers | `SUB_KERNEL_RTDENB` | `0x20` | no (dead `set_rtdenb()` only, via the ASCII string `"20"`, not this macro) |
| Subcommands | `SUB_KERNEL_SUB_CMD_READ` | `0x00` | yes |
| Subcommands | `SUB_KERNEL_SUB_CMD_WRITE` | `0x01` | yes |
| Subcommands | `SUB_KERNEL_SUB_CMD_READ_BSR` | `0x02` | yes |
| IR ack | `SUB_KERNEL_JTAG_IR_ACK` | `0x31` | yes |

### Failure handling

A failed gate in IDCODE or USERCODE logs an error and returns
`STATUS_ERROR`, which `init_jtag()` ignores. A failed gate in the tool-ROM
sequence abandons the rest of that sequence, and `init_jtag()` ignores that
too. `execute()` then calls the empty `read_mem()` or `write_mem()` and
reports success.

### Dead code

`set_rtdenb()` would have sent ASCII TAP-shift strings. `write_jtag_ir()`
built an IR write (`4b051f` TAP reset, `4b0303` to Shift-IR, `3b04<code>`,
`6b0001`/`7b0001` for the last bit, `4b0101` to Run/Idle, a trailing `0D`).
`write_jtag_dr()` built a DR write (`4b0700` idle 8 clock cycles, `4b0201`
to Shift-DR, `3b1e<data>`, `6b0001`/`7b0001` for the last bit, `4b0101`,
`0D`). `read_jtag_dr()` built a DR read (`4b0700`, `4b0201`, `6b1f80000000`
to read 32 bits, `4b0101`, `0D`). The end bit came from the first hex digit
of the code or data string: `write_jtag_ir()` tested it against `0x2`,
`write_jtag_dr()` against `0x8`. `read_response()` looped
`read_serial_data()` until an empty reply, kept only the last non-empty one,
took `mid(4, [3])` of it — the bytes from index 4, for a length read from
the byte at index 3 — and reversed that slice byte-for-byte. `set_rtdenb()`'s
one call is commented out; it would have written IR code `"20"` (`RTDENB`),
then DR `"00000001"`, then read the DR back.
