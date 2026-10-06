# Subaru Denso SH705x K-Line bench checklist

## 0. STOP — not hardware-qualified

Do not treat this family as proven until each MCU (SH7055, SH7058) has passed
every section below on real hardware. Record adapter make, model, firmware,
interface mode, ECU part number and identifier, MCU, protocol variant, date,
operator and result. Keep separate records per MCU; evidence from one does not
qualify the other.

## 1. TestWrite safety premise — first, before any real write

- With a trusted full-ROM dump in hand, run TestWrite on an image that differs
  in one block. Capture the serial trace.
- Confirm the trace shows `FLASH_DISABLE` (0x21) acknowledged, then
  `BLANK_PAGE` (0x25), `WRITE_FLASH_BUFFER` (0x22) and `VALIDATE` (0x23) — and
  never `FLASH_ENABLE` (0x20) or `COMMIT` (0x24).
- Read the ROM again and confirm it is byte-identical to the trusted dump.
  If anything changed, stop: TestWrite is not a dry run on this kernel.

## 2. Startup

- Confirm the adapter reset happens before configuration, then 4800 baud.
- Cold start: the kernel probe at 62500 gets no reply, the SSM handshake
  (`BF`, `81`, `83`, `27 01`, `27 02`, `10 85 02`) succeeds, the kernel
  uploads at 15625 and answers its ID at 62500.
- Warm start with the kernel still running: the probe succeeds and no
  handshake or upload is sent.
- Repeat with an `_ecutek` protocol on an EcuTek-flashed ECU.

## 3. Read

- Read the full ROM and compare it byte-for-byte with a trusted image.
- Confirm the page reply layout the port requires: `BE EF`, length, `0x43`,
  0x400 data bytes, trailing sum8. If the kernel's trailing byte is not a
  sum8, record it and stop.
- Confirm the saved ROM ID is the ECU ID hex followed by `_`.

## 4. Write and recovery

- Write an image differing in one small block; confirm only that block is
  erased and rewritten and that the post-compare shows no differences.
- Interrupt power to the adapter mid-write on a sacrificial ECU, then confirm
  a warm-start write (kernel still running) recovers it.
