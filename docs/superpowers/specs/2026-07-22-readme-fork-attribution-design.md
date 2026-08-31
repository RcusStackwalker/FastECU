# README fork attribution rewrite

## Problem

This repo (`RcusStackwalker/FastECU`) is a fork of `miikasyvanen/FastECU` that
has significantly diverged (Bazel build, Mitsubishi M32R support, macOS
support, protocol/backend rewrites). The current `README.md` still reads as
if it *is* the original project: no mention of the fork lineage, and it
carries the original author's personal PayPal donation link and
`fastecu.fi` support forum as if they belong to this project ("us", "our
free time").

Goal: rewrite `README.md` to clearly acknowledge the base project and its
original contributors, drop the misattributed donation/support content, and
otherwise leave the current, fork-specific documentation untouched.

## Scope

- `README.md` only. `USING.txt` was reviewed and found already fork-neutral
  (generic build/usage/safety notes, releases link already points to
  `RcusStackwalker/FastECU`) — no changes needed there.

## Changes

1. **Fork notice**, added directly under the `# FastECU` title as a
   blockquote:
   - States this is an independently maintained fork of
     `miikasyvanen/FastECU`.
   - States it is not affiliated with or endorsed by the original project
     or author.
   - Notes, parenthetically, that a project rename is planned.

2. **Remove misattributed donation/support block** (current lines ~30-35:
   PayPal link, `info@fastecu.fi` email, `fastecu.fi` forum link, "If you
   find FastECU useful, please consider supporting us..." paragraph). None
   of this belongs to this fork.

3. **Restructure the acknowledgments section** into two groups:
   - Credit to `miikasyvanen/FastECU` as the upstream project this fork is
     built on, with a note that its own donation/support channels are
     linked from the upstream repository for anyone who wants to support
     the original work.
   - The existing pre-fork upstream credits (nisprog/fenugrec, rimwall,
     SergArb, alesv, jimihimisimi) for the SH-2 kernel work, kept as-is
     under a clarified heading — these predate the fork and are unrelated
     to fork attribution.

4. **No changes** to: build/test instructions, clang-tidy section,
   supported ECU/TCU model lists, MUT/DMA logging section, unbrick section.
   These are current, fork-specific content, not stale attribution.

5. License block (GPLv3 summary) stays; only the donation paragraph
   immediately after it is removed.

## Out of scope

- Actually renaming the project (deferred; only a note that it's planned).
- Touching `LICENSE`, `docs/adr/*`, bench checklists, or
  `3rdparty/hardware/README.md` — not in the reviewed scope per user
  decision.
- Creating a separate `AUTHORS`/`NOTICE` file — acknowledgment stays inline
  in `README.md`, matching the project's existing convention (no such file
  exists today).
