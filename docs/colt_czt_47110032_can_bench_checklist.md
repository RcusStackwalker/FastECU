# Colt CZT (Z37A, 47110032) CAN reflash — bench qualification checklist

Desktop composition now uses the ordered `FlashWorkflowFactory` registry and
common `FlashDialog`; future portable migrations add a registration rather
than a family dialog or `MainWindow` branch. Hardware qualification status is
unchanged.

Gate before any real-vehicle use of the Colt CAN workflow. Qualify all four
selectable protocols independently:

- 384 KiB factory security: `mitsu_ecu_m32r_can`
- 384 KiB vendor security: `mitsu_ecu_m32r_can_vendor_ext`
- 512 KiB factory security: `mitsu_ecu_m32r_can_512kb`
- 512 KiB vendor security: `mitsu_ecu_m32r_can_vendor_ext_512kb`

This follows the convention of the parent research repository's
`mmc-patches/m32r/39670016/z27a_mt_audm/mode23-bench-notes.md`. Do not skip
steps. Have an independently verified recovery procedure and tool available
before either write qualification. A separate `boot-talk` utility may exist in
the parent workspace, but it is not part of this repository.

0. **Put the bench ECU on a stock ROM before qualifying the factory-security
   protocols.** `mitsu_ecu_m32r_can` and `mitsu_ecu_m32r_can_512kb` authorize
   with factory security, which only a stock image answers; an ECU still
   running a vendor-extension-patched ROM cannot qualify them, and a pass
   recorded against a patched image proves nothing about the factory path.
   Flash a known-good stock `47110032` from the parent research repo first
   (`mmc-research/m32r/47110032_z37a_mt_2005/`), confirm it boots to
   userspace, and qualify the two factory-security protocols against it.
   Re-flash the vendor-extension image afterwards to qualify
   `mitsu_ecu_m32r_can_vendor_ext` and `_vendor_ext_512kb`. Record which image
   was resident for every result below.

1. **Connect and identify each selector.** Power a bench/spare Z37A ECU and
   select each of the four Mitsubishi / Colt CZT entries in turn. Confirm the
   UI reports the selected 384 KiB or 512 KiB capacity, `connect_bootloader()`
   enters bootload session `0x85`, and the normal IDs complete factory security
   while both `vendor_ext` IDs complete the vendor authorization handshake.

2. **Qualify 384 KiB reads separately.** For each 384 KiB protocol, choose
   Read and save the result. Confirm:

   - the file is exactly `0x60000` bytes (384 KiB);
   - the first request starts at ECU address `0x00000` and no request reaches
     or crosses `0x60000`;
   - file offsets are zero-based ECU addresses: compare known bytes across the
     `0x00000` boot region, the `0x08000` userspace boundary, and the final byte
     at `0x5FFFF` against an independently known-good 384 KiB dump; and
   - repeating the read produces the same SHA-256 hash.

3. **Qualify 512 KiB reads separately.** For each `_512kb` protocol, choose
   Read and save the result. Confirm:

   - the file is exactly `0x80000` bytes (512 KiB);
   - the first request starts at ECU address `0x00000` and no request reaches
     or crosses `0x80000`;
   - file offsets are zero-based ECU addresses: compare known bytes at
     `0x00000`, `0x08000`, the top-region boundary `0x60000`, and the final byte
     at `0x7FFFF` against an independently known-good 512 KiB dump; and
   - repeating the read produces the same SHA-256 hash.

4. **Qualify 384 KiB writes separately on the bench ECU.** For each 384 KiB
   protocol, select Write with a known-good file exactly `0x60000` bytes.
   Confirm files of `0x5FFFF` and `0x60001` bytes are rejected before any
   confirmation or worker starts. Confirm the warning says file bytes
   `0x00000`-`0x08000` are ignored, the ECU bootloader stays unchanged, and
   only `0x08000`-`0x60000` is written and verified. Then confirm:

   - no top-128-KiB bootstrap confirmation, comparison read, redirect-helper
     upload, or request at/above `0x60000` occurs;
   - the stock erase-page and write-page routine uploads to `0x805568` and
     `0x8054AC` complete;
   - every source byte keeps its absolute file offset (file offset `0x08000`
     targets ECU address `0x08000`, with no userspace re-basing);
   - write progress is monotonic on one `0/0x58000` through
     `0x58000/0x58000` scale; and
   - the immediate byte-for-byte read-back verifies `0x08000`-`0x60000`, with
     no read, erase, or write request below `0x08000`.

5. **Qualify 512 KiB writes separately on the bench ECU.** For each `_512kb`
   protocol, select Write with a known-good file exactly `0x80000` bytes.
   Confirm files of `0x7FFFF` and `0x80001` bytes are rejected before any
   confirmation or worker starts. Confirm the warning says file bytes
   `0x00000`-`0x08000` are ignored, the ECU bootloader stays unchanged, and
   the aggregate write/verify range is `0x08000`-`0x80000`. Confirm source file
   offsets remain absolute ECU addresses throughout. Then exercise both
   conditional-bootstrap branches:

   - **Mismatch/bootstrap:** On an ECU whose `0x60000`-`0x80000` region differs,
     confirm the pre-execution bootstrap warning was accepted, the internal
     comparison detects the mismatch, both redirect routines upload to
     `0x805568`/`0x8054AC`, the carrier-window erase/write completes, and the
     immediate re-read reports `Top 128KB verified`.
   - **Match/skip:** Re-run with the same ROM and confirm `Top 128KB already
     matches, no bootstrap needed`, with no redirect upload or additional
     erase trigger.
   - In both cases, confirm the stock userspace write and immediate read-back
     verify `0x08000`-`0x60000`, aggregate progress is monotonic on one
     `0/0x78000` through `0x78000/0x78000` scale, and no read, erase, or write
     request targets below `0x08000`.

6. **Exercise cancellation and recovery for each capacity.** Confirm the main
   erase warning appears before worker startup and before its
   `ServiceRequestReflash` (`0x3B`) unlock. For 512 KiB, distinguish this from
   the conditional-bootstrap unlock. Confirm cancellation immediately after
   either applicable erase is reported without another write attempt, and
   recover the ECU using the verified procedure before continuing.

7. **responsePending (NRC 0x78) handling.** The erase and CRC-check routines
   are the exchanges most likely to make the ECU report responsePending.
   Confirm the operation completes rather than aborting, and that the debug
   log shows one "responsePending" line per absorbed reply. This path has no
   hardware evidence behind it; it was added with the UDS layer and is
   verified only by scripted tests.

8. **CAN reply-id validation.** `CanFlashUdsChannel` now rejects any reply
   frame whose arbitration id does not match the expected response id;
   before this branch such a frame was accepted silently. Confirm normal
   operation across all four protocols produces no spurious reply-id
   rejections in the debug log.

9. **Absorbed-pending exchange duration.** `pending_timeout_ms = 3000` and
   `max_pending_repeats = 10` are `ExchangePolicy` defaults that **no call
   site on this path overrides** — the executor only ever sets
   `read_timeout_ms`. Every exchange can therefore absorb ten
   responsePending replies before giving up, and any one of them can take
   `read_timeout_ms + 10 × 3000 ms` to fail:

   - about **30 seconds** (`500 ms + 10 × 3000 ms`) at every exchange that
     uses the default 500 ms read timeout, which is all of them bar the three
     below; and
   - about **33 seconds** (`3000 ms + 10 × 3000 ms`) at the three that use the
     extra-long 3000 ms read timeout: the reflash-unlock request, the erase
     trigger, and the CRC check.

   Do not read those three as the only places a long stall can happen. The
   exchange an operator is most likely to meet one at is TransferData: a
   384 KiB userspace write sends about 1,400 of them (`0x58000` bytes at 256
   bytes per frame) and the read-back verify makes about 1,900 more chunk
   exchanges (192 bytes each), and any single one can sit for ~30 s. Expect
   the UI to appear stalled for that long anywhere in the run, and wait rather
   than pulling power — an interruption mid-TransferData is as capable of
   bricking the unit as one mid-unlock or mid-erase.

10. **ROM checksum correction (`ChecksumEcuMitsuM32rCan`).** Saving a
    calibration now rewrites the balance word at `0x3FFC0` so the ECU's own
    flash CRC checker accepts the image; a wrong value here boots to a stored
    ROM-checksum DTC (P0606) instead of userspace, so qualify it before
    trusting any modified image.

    Automated evidence already in the tree, which this step exists to confirm
    on hardware rather than repeat:

    - `checksum_ecu_mitsu_m32r_can_test` models the ECU's own checker
      (`flash_crc_check_block` @ `0x4B1BC`, `flash_crc_check_block_2` @
      `0x4B310`, `rom_crc_finalize` @ `0x4B0AC`) and asserts corrected images
      satisfy it, including the second-pass agreement check;
    - against the parent research corpus, both stock `47110032` dumps verify
      untouched, `colt_injected.bin` and 100 randomised corruption trials all
      correct to an ECU-accepted image, and every correction is confined to
      `0x3FFC0`-`0x3FFC3`.

    On the bench, for each capacity:

    - load a stock image, confirm saving reports no correction and leaves the
      file byte-identical (SHA-256 unchanged);
    - modify one calibration byte, save, and confirm exactly the four bytes at
      `0x3FFC0` change besides the byte edited;
    - write that corrected image, power-cycle, and confirm the ECU reaches
      userspace, idles, responds to diagnostics, and stores **no** ROM
      checksum DTC. Read stored codes explicitly rather than relying on the
      MIL;
    - repeat with the checksum correction deliberately skipped (edit a byte
      and flash without saving through the correction path) and confirm the
      ECU **does** raise the ROM checksum DTC. Without this negative result a
      pass only shows the ECU is not checking, not that the correction is
      right. Recover by re-flashing the corrected image.

11. **Post-write validation.** After each capacity/security combination,
    power-cycle the ECU, repeat the matching full zero-based read, and
    compare its SHA-256 hash and the capacity-boundary sample bytes with the
    intended image. Record date, ECU identity, adapter identity, selected
    protocol, operator, image hash, read-back hash, and pass/fail result.

12. **Only after Steps 0-11 pass repeatably for both capacities and both
    authorization variants on a bench/spare ECU**, consider use on a real
    vehicle.
