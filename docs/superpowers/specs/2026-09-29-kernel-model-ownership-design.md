# 6n-2 Kernel Model Ownership Design

## Intent and scope

Complete the kernel-model part of desktop closure by moving the last portable headers out of `src/backend/definitions`. The result should make flash ownership explicit and retire that obsolete package without changing flash behavior. This slice is an ownership relocation, not a redesign of the flash device table or wire protocol. The next slice, 6n-3, moves the Qt byte-conversion helper to a desktop boundary; Android work follows desktop closure.

## Current state

`src/backend/definitions:models` exports `kernelmemorymodels.h` and `kernelcomms.h`. The former holds the global `mcu_type`, block and device structures, inline constexpr per-device block arrays, and the `flashdevices` table. Flash lookup, plans, executors, and tests include it; Bazel targets under `src/backend/flash` depend on `:models`. The latter contains kernel command and error-code macros. No active source directly includes it, but it is still part of the exported `:models` target. The package name implies definition-catalog ownership even though its remaining contents serve flash workflows.

## Decision

Place both headers under `src/backend/flash/kernel`, with separate header-only targets: `:memory_models` and `:commands`. Keep the current filenames, declarations, macro names and values, array contents and order, and global namespace in this slice. `:memory_models` is the dependency for flash targets that use device/block data. `:commands` retains the kernel command definitions as a portable flash-owned target for future use, without adding an artificial dependency to consumers that do not include it. Restrict visibility to the backend layer and above, matching the current package's effective access. Do not add compatibility forwarding headers or an alias under `src/backend/definitions`; retire the directory and its `:models` target once consumers migrate.

This is preferable to splitting protocol constants into algorithms now: there are no current direct include consumers to establish that boundary, while the header documents flash-kernel wire values. A typed/namespaced API may be worthwhile later, but it would change the surface used by flash code and obscure whether this relocation preserved behavior.

## Migration and data flow

Move the two headers without editing their contents. Update all active includes of `kernelmemorymodels.h` to the new path and replace `//src/backend/definitions:models` dependencies with `//src/backend/flash/kernel:memory_models`. Update source and test comments that cite the old path where they are intended as navigable references. Do not change device lookup or duplicate literal constants in executor code: those are separate behavior and consolidation decisions. `flash_device_lookup` continues to expose its pointer and index forms; callers continue to see the same `flashdevices` table and sentinel.

Bazel's portable closure remains `src/backend/flash` → flash-owned portable kernel headers; no platform or Qt dependency is introduced. The table's existing global inline constexpr storage and pointer relationships remain intact. No runtime initialization, persistence, configuration, or UI flow changes are part of this slice.

## Failure modes and compatibility

A missed include or dependency produces a build failure, so remove the old package only after all active consumers have migrated. The higher-risk error is an accidental alteration to a flash block boundary, device name, table order, enum value, command value, or terminal sentinel. Compare the moved headers byte-for-byte with their pre-move versions and rely on existing lookup and flash plan/executor tests for behavioral coverage. Retain known incomplete or surprising model entries unchanged; corrections need their own evidence and review.

External source including the old repository path would need to update its include. This repository does not publish those headers as a stable external SDK, so an in-repo migration with no compatibility shim is appropriate.

## Verification and exit criteria

- Confirm both moved headers are byte-for-byte identical to their prior versions, apart from repository paths in consumers and documentation.
- Confirm no active include or Bazel dependency refers to `src/backend/definitions`, and the old package is gone.
- Run Gazelle consistency; build and test affected flash targets, then release `//...` and `//:portable_closure` with the desktop application.
- Run the repository's required lint/static checks for changed files and record platform-specific skips or qualification limits accurately.
- Keep 6n open until 6n-3 completes. Hardware, packaging, and Windows/Linux runtime qualification are separate gates; a structural relocation alone does not establish them.
