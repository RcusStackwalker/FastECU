# Colt CZT (Z37A, 47110032) CAN reflash — bench qualification checklist

Gate before any real-vehicle use of `FlashEcuMitsuM32rCan` (protocol
`mitsu_ecu_m32r_can`). Same convention as this project's other
bench-qualification gates (e.g. `mmc-patches/m32r/39670016_z27a_mt_audm/mode23-bench-notes.md`).
Do not skip steps. Have a recovery path (this project's `boot-talk`
utility) available before Step 4.

1. **Connect.** Power a bench/spare Z37A ECU, select "Mitsubishi / Colt CZT
   / Z37A 5MT" in FastECU, choose Read. Confirm `connect_bootloader()`
   completes (diagnostic session ok) without security access (read uses
   the basic session).
2. **Full read.** Read the full 384KB image. Compare byte-for-byte against
   a known-good reference dump of the same ROM (`47110032`, per
   `mmc-definitions/roms/m32r/47110032/rom.toml`).
3. **Write-path dry run on the bench ECU (not a vehicle).** Select Write
   with a known-good ROM file. Confirm the erase-page and write-page
   routine uploads each complete (`upload_and_commit` returns true for
   both `0x805568` and `0x8054AC`).
4. **Erase trigger — highest risk step.** Confirm the in-app warning
   dialog appears before the `ServiceRequestReflash`(0x3B) unlock frame is
   sent. Only proceed past it with the bench ECU connected and a recovery
   path on hand. If the bootloader locks up (matching the original
   author's experience in `externals/livemonitor`), use `boot-talk` to
   recover before continuing.
   - **Worker-thread re-verification (phase 1b pattern-proof).**
     `FlashEcuMitsuM32rCan` is the pilot module for the flash-operation
     worker-thread migration (see
     `docs/superpowers/specs/2026-07-03-flash-operation-worker-design.md`).
     This step is the one place `write_mem()` shows a mid-operation
     `confirm()` prompt (the erase-trigger warning) while the flash
     operation itself now runs on a worker thread. Confirm on real
     hardware that the QEventLoop-based `run()` still correctly services
     that `confirm()` dialog — i.e. the UI remains responsive and the
     dialog is answerable — while the worker thread is blocked waiting on
     it, and that it isn't skipped, double-fired, or answered against a
     stale state. Also confirm the progress bar updates live during the
     full multi-minute read/write in Steps 2, 3, and 5, not just at
     start/end.
5. **Full write + read-back.** Once the erase trigger succeeds, confirm
   the userspace write (`0x8000`-`0x60000`) completes, then re-read the
   full ROM and diff against the file that was written.
6. **Only after 1-5 pass repeatably on a bench/spare ECU**, consider use
   on a real vehicle.
