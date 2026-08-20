# Agent-drivable bench CLI for Colt CZT CAN reflash

## Problem

Erasing the top region of a Colt CZT (Z37A, ROM 47110032) fails on the bench.
The wire evidence is one exchange:

```
Sent:     00 00 07 e0 31 e0
Response: 00 00 07 e8 71 e0 01
(EE) Erase trigger (top 128KB bootstrap) reported failure (status 0x01)
```

Diagnosing this needs many short, stateful experiments against a real ECU.
The only client today is the desktop GUI, which gates every destructive step
behind a `ConfirmationSpec` dialog, runs one fixed phase sequence, and cannot
be driven unattended. Every hypothesis therefore costs a human-in-the-loop
GUI round trip, which is why the investigation has stalled at the point of
guessing.

This spec covers a small CLI that makes those experiments cheap. The
diagnosis itself is the CLI's first use, described under
[First investigation](#first-investigation), not part of the deliverable.

### What the ROM says about status 0x01

From `colt_commented.S` in `mmc-research/m32r/47110032_z37a_mt_2005/`, the
byte `cobd_data[2] = 1` is written at `0x5a28`, reachable by two paths that
mean opposite things and are indistinguishable on the wire:

- **`0x59b0`, the pre-erase gate.** When `!(fp58_f16 & 0x40) && !flash200_u8`,
  the handler replies status 1 having never called the erase.
- **`0x5a14`-`0x5a20`, post-erase.** `flasher_try_erase_range_call` (`0x40f4`)
  returned 3, meaning `memory_erase_flash_range` reported failure.

The second path narrows further. `erase_flash_range` (`0x620` in ROM, RAM copy
at `0x805300`) returns only:

| value | meaning |
| --- | --- |
| `0xfffd` | range precondition failed: `p_begin` not a block begin, or `l_last_byte` not a block end, or either above `0xbffff` |
| `0xfffe` | cumulative time in memory calls exceeded 350000 — **retried** by the `while (ret == -2)` loop in `flasher_try_erase_range_call`, so it is not a terminal value |
| `0xffff` | some `erase_block_memfun(memory_block_edges[i].end)` returned a value other than 1 |

`p_begin` and `p_size` are hardcoded `0x8000` and `0x58000` at `0x59b4`-`0x59c4`,
independent of which routine was uploaded to RAM. So if the ordinary
non-redirect erase succeeds, `0xfffd` cannot explain a redirect-path failure,
and `0xfffe` is not terminal. **The remaining explanation is `0xffff`: the
uploaded redirect routine returned something other than 1 for at least one
block.**

`erase_redirect32170.S` returns 0 on exactly one path — the FBUSY wait loop
exhausting its 24,000,000 iterations. The leading hypothesis is therefore that
`erase_block_memfun` receives `memory_block_edges[i].end` and the redirect adds
a flat offset to it; if the redirected address is not a valid block end within
`0x60000`-`0x80000`, the flash controller never completes and FBUSY never
clears. M32R flash blocks are non-uniform, so this is not guaranteed by
construction. The block table is readable at runtime, so the hypothesis is
directly testable.

### Addressable ECU state

`frame_pointers.txt` for this ROM maps `0x800000` to `fp-32768`, giving
**FP = 0x808000**. Every variable in the erase handler is consequently
reachable through ordinary `ReadMemoryByAddress` (`0x23`):

| symbol | expression | address |
| --- | --- | --- |
| `flash200_u8` | flash `0x200` | `0x000200` |
| `fp58_f16` | `fp-58` | `0x807FC6` |
| `can_flasher_block_state` | `fp-120` | `0x807F88` |
| `memory_block_edges[]` | `fp-10584`, 8 bytes per entry | `0x8056A8` |

### A second, independent discrepancy

`kEraseRedirectRoutine` in `src/algorithms/protocol/colt/mitsu_colt_can_protocol.cpp`
does not match what `mmc-patches/m32r/47110032/reflash/erase_redirect32170.S`
currently assembles to. They are two generations of the same routine:

| | `erase_redirect32170.bin` | `kEraseRedirectRoutine` |
| --- | --- | --- |
| guard | `seth r3,#1` / `bc`, then `seth r3,#3` / `bnc` — window `[0x10000,0x30000)` | `seth r3,#2; or3 r3,r3,#0x8000` / `bnc` — window `[0,0x28000)` |
| offset | `seth r2,#5` — `+0x50000` | `seth r2,#5; or3 r2,r2,#0x8000` — `+0x58000` |
| implied carrier | `0x10000` | `0x8000` |
| filler tail | `e2 88 88 c9` | `e2 00 00 88` |

Both map the erase sweep onto `0x60000`-`0x80000`, and the executor's carrier
is `kUserspaceStart` (`0x8000`), which matches the array rather than the `.S`.
Neither is self-evidently wrong. The problem is that the header comment cites
the `.S` as the provenance of bytes that no longer come from it, so the two
cannot be reconciled by reading. The CLI resolves this on hardware instead.

## Design

### Structure

Chosen over two alternatives: a `cc_binary` inside the existing transport
package (smaller diff, but puts an executable in a platform library package and
gives the `SerialPortActions` coupling a second customer), and a Qt-free CLI
over a fresh `J2534_unix.cpp` adapter (stays in `//:portable_closure`, but
reimplements timing, retry and ISO-TP wiring, so a CLI result would stop being
evidence about the desktop app's behaviour — disqualifying for a debugging
instrument).

**1. New factory in `//src/platform/desktop/common/transport:flash_transports`**,
`desktop_transport_factory.{h,cpp}`:

```cpp
struct DesktopCanTransportConfig {
    std::string port_name;      // empty -> first detected device
    std::string peer_address;   // empty -> local J2534
    std::string peer_password;
};

Result<std::vector<std::string>> list_desktop_serial_ports();
Result<std::unique_ptr<ICanFlashTransport>> open_desktop_can_flash_transport(
    const DesktopCanTransportConfig&, const Iso15765Config&);
```

The body is the sequence `MainWindow` performs by hand today: construct
`SerialPortActions`, `check_serial_ports()`, `set_serial_port()`, wrap in
`DesktopCanFlashTransport`'s **owning** constructor, `configure()`, `open()`.
`DesktopCanFlashTransport::configure()` already performs every CAN setter, so
the factory adds only device selection and typed failure.

This exists so the CLI never includes `serial_port_actions.h`. A direct
dependency on `serial_qt_compat` would require adding `//apps/bench` to that
target's visibility list, which `//:serial_compat_allowlist` freezes as *may
shrink, never grow*. Only the `transport` package's own `default_visibility`
grows, which is not frozen. The factory is also the seam the desktop app will
need when `MainWindow`'s direct `new SerialPortActions` is untangled in step 6.

**2. `//apps/bench:fastecu-bench`** — a Qt `cc_binary` using `QCoreApplication`
only, no GUI:

| file | responsibility | testable without hardware |
| --- | --- | --- |
| `main.cpp` | argument parsing and dispatch, nothing else | not tested — dispatch only |
| `bench_session.{h,cpp}` | owns the transport, `CanFlashUdsChannel`, `UdsClient`; provides `connect()` and `exchange(pdu, policy)` | no |
| `bench_commands.{h,cpp}` | one function per subcommand, producing `CommandOutcome{tx, rx, elapsed, vbatt, verdict, notes}` | yes |
| `bench_format.{h,cpp}` | renders `CommandOutcome` as text or JSON | yes |

`bench_commands` takes an `IBenchSession` interface rather than the concrete
session, so command construction and reply decoding are unit-testable against a
fake. `bench_commands` and `bench_format` are Qt-free by construction.
`bench_session` holds nothing but wiring, because it is the only part CI cannot
reach.

Ports come from `src/platform/desktop/common/ports/`, plus a CLI `IEventSink`
writing to stderr and a SIGINT-backed `ICancellationToken`.

### Command surface

**Steps chain within a single process, and this is load-bearing rather than a
convenience.** Each process establishes one bootloader session; reconnecting
re-runs `0x10` and `0x27`, which plausibly resets the very `fp58_f16` state
under investigation. Running `unlock` and `erase` as two invocations would
therefore not reproduce what the desktop app does.

```
fastecu-bench unlock --destructive : erase --destructive
fastecu-bench --script -            # steps on stdin, one per line
```

| subcommand | wire | destructive |
| --- | --- | --- |
| `ports` | none | |
| `connect` | `0x10` session + `0x27` seed/key; implicit first step unless `--no-connect` | |
| `read <addr> <len>` | `0x23`, chunked at `kFlashReadBlockSize` | |
| `dump <addr> <len> <file>` | `0x23` into a file | |
| `crc-check <addr>` | `0x31/225` | |
| `send <hex...>` | PDU through `UdsClient` | |
| `send-raw <hex...>` | bypasses `UdsClient` echo and NRC handling; reports raw bytes | |
| `unlock` | `0x3B` | yes |
| `erase` | `0x31/224` | yes |
| `download <addr> <file>` | `0x34`, `0x36`..., CRC transfer, `0x31/225` | yes |
| `upload-routine <name>` | `download` to that routine's RAM slot | yes |

`<name>` is one of `erase-redirect`, `write-redirect`, `erase-page`,
`write-page`, resolving to the baked arrays. `--from <file>` substitutes a
freshly assembled `.bin`, which is how the provenance discrepancy above gets
tested on hardware without editing C++.

Global flags: `--port <name>`, `--json`, `--verbose`, `--timeout <ms>`,
`--keep-going`, `--no-connect`.

Replies carry decoded notes rather than bare hex. `71 e0 01` renders as
`status=0x01 -> colt_commented.S 0x5a28, reachable from the pre-erase gate
(0x59b0) or erase-routine failure (0x5a14); ambiguous`.

### Error handling

Every operation returns `Result<T>`/`Error` on the existing seven `ErrorKind`
values. No new kinds: nothing here needs one, and adding one would require
amending the [step-5 design](2026-07-22-step5-backend-portable-design.md).

- Exit code is 0 on success, or a distinct nonzero per `ErrorKind`, so an agent
  can branch without parsing prose.
- `--json` writes one object per step to stdout and all logging to stderr.
- A failed step **aborts the remaining chain** by default. With ECU state in
  play, continuing past a failure produces misleading evidence. `--keep-going`
  overrides.
- `--destructive` is validated **at parse time, before the port is opened**, so
  a chain whose third step is ungated fails before connecting and unlocking
  rather than halfway through.
- Each outcome records `read_vbatt()` alongside tx and rx. The failing log
  shows 11.676 V; brownout during erase is a plausible enough failure mode to
  keep logged, even though the analysis above does not point there.

`--destructive` is per-invocation and the agent supplies it itself, so it
documents intent rather than constraining an agent. The safety story rests on
this being a bench ECU with a `boot-talk` recovery path. That is a deliberate
choice, recorded here so it is not mistaken for an oversight: `0x3B`'s payload
is the one whose original author annotated it "caused bootloader lockup".

### Testing

- `bench_commands` and `bench_format` are tested with `fastecu_portable_gtest`
  against a fake `IBenchSession` in `apps/bench/testing/`, one
  `cc_library(testonly = True)` per mock per
  [ADR 0008](../../adr/0008-use-package-owned-mocks.md). Table-driven: each
  subcommand asserts the exact PDU built; each decoder asserts its reading of
  representative replies, including `71 e0 01` and its ambiguity note.
- The `MitsuColtCan` builders keep their existing suites; this adds no
  duplicate coverage of them.
- The factory gets a test for device-selection failure paths (no devices, named
  device absent) using the existing serial test doubles.
- CI never touches hardware. Manual qualification lives in a new
  `docs/bench-cli-checklist.md`, per the hardware-facing rule in `CLAUDE.md`.

## First investigation

Not part of the deliverable; recorded because it is what the CLI is for and it
determines whether the command surface above is sufficient. Each item is one
invocation.

1. `read 0x200 1` — if `flash200_u8` is nonzero, the pre-erase gate at `0x59b0`
   cannot have fired, so status 1 came from `ret == 3` and the erase really was
   attempted. Cheap, decisive, read-only.
2. `read 0x8056a8 256` — dump `memory_block_edges` and check whether
   `end + 0x58000` values land on real block boundaries in `0x60000`-`0x80000`.
   Tests the leading hypothesis with no destructive step.
3. `upload-routine erase-page : unlock --destructive : erase --destructive` —
   the ordinary non-redirect path as a control. If plain erase returns status 0
   while the redirect returns 1, the routine is implicated and the gate is
   exonerated.
4. `read 0x807fc6 2 : unlock --destructive : read 0x807fc6 2` — establishes
   whether `0x3B` actually sets `fp58_f16 & 0x40`.
5. `upload-routine erase-redirect --from <erase_redirect32170.bin>` against the
   baked array — resolves the provenance divergence on hardware rather than by
   argument.

Probes 1 and 2 are read-only and may answer the question outright.

## Out of scope

- Any change to `MitsuColtM32rCanExecutor`, the `MitsuColtCan` builders, or the
  baked routine arrays. This spec adds an instrument; whatever the
  investigation concludes is a separate change with its own tests.
- A persistent daemon or interactive REPL. Step chaining covers the stateful
  case that motivated it.
- K-Line, SSM, or the other CAN families. The `send`/`send-raw` escape hatch is
  generic, but the named subcommands are Colt CZT only until there is a second
  caller.
- Registration in `PORTABLE_ROOTS`. `bench_commands` is Qt-free and tested as
  such, but `//:portable_closure` guards backend targets, not app packages.
