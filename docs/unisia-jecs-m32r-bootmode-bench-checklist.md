# Subaru Unisia Jecs M32R bootmode bench checklist

## 0. STOP — not hardware-qualified

Do not treat this family as proven until every section below has passed on
real hardware. Record the adapter (make, model, firmware; OpenPort 2.0 or
not), how VPP and MOD1 are supplied, the ECU part number and ECU ID, the
protocol selected, date, operator and result.

## 1. Read (`_20_bootmode` / `_30_bootmode`)

Bootmode Read is the [Unisia Jecs M32R K-Line](unisia-jecs-m32r-bench-checklist.md)
read. Run that checklist's section 1 once per bootmode protocol.

## 2. Kernel upload (attempt 1)

- Confirm the "Connect VPP and MOD1" prompt appears before any K-Line traffic,
  and that declining it sends nothing.
- Capture the upload at 39063 baud, even parity. Confirm each 128-byte chunk's
  local echo is drained on a direct serial adapter and that no bytes are lost.
- Record what, if anything, the ECU sends in the 200 ms after the upload.

## 3. Between attempts

- The executor drops both LEC lines when the upload ends, and the connection
  resets before programming. Legacy's port close did the same implicitly.
  Confirm the kernel is still running afterwards: attempt 2's `AF 31` must be
  answered by `EF 42`.
- Confirm Cancel on the "Remove MOD1" prompt stops before erase, and that the
  VPP notice follows.

## 4. Erase and program (attempt 2, bench ECU with a recovery path)

- Record how long `EF 42` and `EF 52` take against the 20 × 500 ms and
  20 × 1000 ms poll budgets.
- Provoke a negative reply (for example, no VPP) and record the frame.
  Confirm the error status sits in the byte after `EF`, as the executor
  assumes, and that `48` is reported as missing VPP.
- Record the exact bytes the ECU sends after the final `AF 69` block, or
  confirm it sends nothing. The executor accepts `EF 52` or silence.
- Read the ROM back and compare it with the written image.
- Close the dialog during programming. Confirm the VPP notice appears, without
  don't-power-off advice, and that power-cycling with MOD1 re-enters boot mode.
