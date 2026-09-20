# Subaru TCU Hitachi M32R CAN (`sub_tcu_hitachi_m32r_can`) — bench qualification checklist

Gate before any use of the `sub_tcu_hitachi_m32r_can` family against real
hardware, bench or vehicle. The portable `SubaruTcuHitachiM32rCanPlan` and
`SubaruTcuHitachiM32rCanExecutor` replaced the legacy Qt operation in wave 6a-2
and are covered by unit tests only; the row in the
[flash qualification matrix](flash-qualification-matrix.md) records the family
as `experimental`, which means **nothing in this family is hardware-qualified**.

Have an independently verified recovery procedure and tool available before any
write attempt. `boot-talk` (J2534 recovery for bricked M32R units) lives in the
parent workspace, not in this repository; "installed somewhere" is not the same
as "tested on this rig today."

## 0. STOP — the erase scope is unresolved. Do not flash past this item.

**This item blocks every write in section 3. It is not a note. Until it is
answered in writing, with evidence, no write of this family may be attempted on
any hardware you are not prepared to destroy.**

- [ ] **Determine what `31 02 01 FF FF FF FF` actually erases.**

      It is not determinable from this repository whether the erase command
      `31 02 01 FF FF FF FF` scopes the erase to blocks 3–10 or erases the whole
      chip. The port is byte-faithful to legacy either way — it sends exactly
      the bytes the legacy operation sent. **If it is chip-wide, blocks 0–2 —
      the bootloader — are erased and then never rewritten**, because the block
      table (`block_modified = {0,0,0,1,1,1,1,1,1,1,1,0,…}` over `M32R_512KB`)
      deliberately excludes them: the write loop opens, programs and checksums
      only blocks 3–10, covering `0x08000`–`0x80000`. That is a brick, and it is
      a brick that no amount of retrying the write will undo.

      This question must be answered before any first live flash of this family.
      Acceptable evidence, in order of preference:

      1. Kernel disassembly or vendor documentation for the TCU's flash kernel
         showing the `0x31 0x02 0x01` routine's address argument handling —
         specifically whether the four `0xFF` bytes are an address/length pair
         that the kernel clamps to the programmable region, or a sentinel that
         selects "all."
      2. A bench observation on a **sacrificial** TCU whose bootloader has been
         independently dumped and whose recovery over `boot-talk` has been
         demonstrated on that same unit beforehand: erase, then read back
         `0x00000`–`0x08000` and confirm it is unchanged rather than `0xFF`.

      Record the answer, the evidence, and the date here before proceeding:

      - Answer (blocks 3–10 only / chip-wide): ________________
      - Evidence: ________________
      - Date and operator: ________________

## 1. Preconditions

- [ ] The device under test is a bench or spare TCU. Never a car.
- [ ] Section 0 is answered, in writing, above.
- [ ] `boot-talk` recovery is available **and tested on this exact rig, now** —
      not installed elsewhere, not tested on a different unit, not "it worked
      last time."
- [ ] Supply voltage is stable and above 12 V for the whole session.
- [ ] The MCU binding is confirmed — see section 4, which must be settled before
      section 2, because it decides which regions get erased and written.

## 2. Read qualification

Reads are non-destructive, but they exercise the corrected transfer window that
has never been seen by a real TCU (section 4). Do these first, and do them
before any write.

- [ ] Connect and confirm the log reports a decoded `TCU ID:` and `CAL ID:`.
- [ ] Read and save. Confirm the saved file is **exactly `0x80000` bytes**
      (524288). The port rejects a page frame whose length is not
      `5 + page_size` with `BadResponse` rather than silently shortening the
      image, so a completed read that is short should be impossible — verify it
      anyway.
- [ ] Confirm the first `0x8000` bytes are zero. The port synthesizes that
      region; it is not read from the TCU, and it is **not** the bootloader's
      real contents. Do not treat a saved image's low region as a bootloader
      backup.
- [ ] Repeat the read and confirm the two files have the same SHA-256.
- [ ] Record the raw first-page and last-page reply bytes, not a paraphrase.

## 3. Write qualification — gated by section 0

Do not begin until section 0 is answered and section 2 has passed.

- [ ] Confirm an image that is not exactly `0x80000` bytes is rejected before
      any transport call.
- [ ] Confirm `test_write` is refused with `Unsupported` before any transport
      call. **There is no dry run for this family.** The legacy "test write"
      threaded a `test_write` argument into `reflash_block` and then never read
      it: it performed a real erase and a real `0xB6` flash write. The portable
      plan rejects the mode outright rather than legitimizing that.
- [ ] Write a known-good image. Confirm from the traffic that no block window,
      data frame or checksum request addresses anything below `0x08000`.
- [ ] Read the TCU back and confirm `0x08000`–`0x80000` matches the written
      image byte for byte.
- [ ] Confirm the unit still boots and communicates after the write.

## 4. VERIFY items carried from the port

Both are open questions that the port could not settle from this repository.
They are listed in the family's
[flash qualification matrix](flash-qualification-matrix.md) row as well.

- [ ] **`M32R_512KB` MCU binding.** The protocol-to-MCU mapping lives in
      EcuFlash-side definition files outside this repository; the only in-repo
      evidence is that the sibling TCU families use the same value. **Its blast
      radius now includes which regions get erased**, not merely what validates:
      the block table this MCU selects is what decides which blocks the write
      opens and, together with section 0, what the erase touches. Confirm the
      binding externally before any write.
- [ ] **The corrected read window.** The port sends `00 80 00` / `07 80 00` —
      the clamp's evident intent, `0x78000` from `0x8000`. Legacy's
      `start_addr - 0x00100000` underflowed to `0xF00000` for the only
      `start_addr` ever passed, and produced a `0x88000`-byte image for a
      `0x80000` ROM. **No TCU has ever been asked for `00 80 00` rather than
      `F0 00 00`.** Confirm the kernel accepts it, and that the returned pages
      are the bytes at the addresses requested.

## 5. Sign-off

Complete only after sections 0 through 4 have all passed.

- [ ] Date: ________________
- [ ] TCU / ROM id: ________________
- [ ] Adapter: ________________
- [ ] Erase scope answer from section 0: ________________
- [ ] Operator initials: ________________
