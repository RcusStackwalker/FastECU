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
2. **Full read.** Read the full 512KB image (`connect_bootloader()` + mode
   `0x23` reads are unrestricted up to the physical flash size, unlike
   writes). Compare byte-for-byte against a known-good reference dump of
   the same ROM (`47110032`, per `mmc-definitions/roms/m32r/47110032/rom.toml`).
3. **Write-path dry run on the bench ECU (not a vehicle).** Select Write
   with a known-good ROM file at least `0x80000` (512KB) bytes. Confirm
   the *stock* erase-page and write-page routine uploads each complete
   (`upload_and_commit` returns true for both `0x805568` and `0x8054AC`)
   — these happen right after the top-128KB bootstrap step below, as
   `write_mem()`'s existing unconditional first two uploads.
4. **Top 128KB bootstrap — second highest-risk step, new in this
   revision.** On a bench ECU whose top 128KB (`0x60000`-`0x80000`) has
   never been written, this step *will* fire (nothing to match against).
   Confirm the "Top 128KB bootstrap" warning dialog appears before
   anything else happens in `write_mem()` — before even the stock
   routine uploads in Step 3. This sends the high-risk erase-trigger
   sequence a **first** time this session (Step 5 below sends it again).
   Confirm:
   - The redirect erase/write routine uploads to `0x805568`/`0x8054AC`
     both complete.
   - The carrier-window erase (declared at `0x8000`-`0x28000`,
     redirected to `0x60000`-`0x80000`) completes without a bootloader
     lockup.
   - The carrier write (128KB via the redirect write routine) completes.
   - The immediate re-read/verify inside `ensureTopRegionWritten()`
     reports a match (log line "Top 128KB verified") before `write_mem()`
     continues to Step 3's stock routine uploads.
   - If the bootloader locks up here, use `boot-talk` to recover before
     continuing — same recovery expectation as Step 5.
5. **Erase trigger (main write) — the original highest-risk step, now
   the *second* erase-trigger of the session.** Confirm the in-app
   warning dialog appears before the `ServiceRequestReflash`(0x3B) unlock
   frame is sent for the *main* write (distinct from Step 4's bootstrap
   unlock). Only proceed past it with the bench ECU connected and a
   recovery path on hand. If the bootloader locks up (matching the
   original author's experience in `externals/livemonitor`), use
   `boot-talk` to recover before continuing.
   - **Worker-thread re-verification (phase 1b pattern-proof).**
     `FlashEcuMitsuM32rCan` is the pilot module for the flash-operation
     worker-thread migration. Steps 4 and 5 both show a mid-operation
     `confirm()` prompt while the flash operation itself runs on a worker
     thread. Confirm on real hardware that the QEventLoop-based `run()`
     still correctly services *both* prompts — i.e. the UI remains
     responsive and each dialog is answerable — while the worker thread
     is blocked waiting on it, and that neither is skipped, double-fired,
     or answered against a stale state. Also confirm the progress bar
     updates live during the full multi-minute read/write in Steps 2, 3,
     4, and 6, not just at start/end.
6. **Full write + read-back.** Once Step 5's erase trigger succeeds,
   confirm the userspace write (`0x8000`-`0x60000`) completes, then
   re-read the full 512KB ROM and diff against the file that was
   written — this should now cover the top 128KB too, since Step 4
   already brought it to the intended content and the main write left
   it untouched.
7. **Re-run on the same bench ECU with the same ROM.** Confirm Step 4's
   top-128KB bootstrap is *skipped* this time (log line "Top 128KB
   already matches, no bootstrap needed", no redirect routine uploads,
   no extra erase-trigger) — proving the match-skip path works, not
   just the mismatch/bootstrap path exercised in Steps 4-6.
8. **Only after 1-7 pass repeatably on a bench/spare ECU**, consider use
   on a real vehicle.
