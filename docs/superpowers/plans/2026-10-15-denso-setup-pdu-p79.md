# Denso setup PDU P79 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Execution method requires user selection; this document authorizes no implementation.

**Goal:** Replace four identical Denso ISO-15765 setup-PDU encoders with one pure operation, preserving every emitted byte and caller policy.

**Architecture:** Add `DensoIso15765SetupPdu` to the existing package-owned common module. The four bootloader-dialect executors call it directly for read download/upload setup and erase download setup. Family code continues to select the service and region and owns transport, response handling and sequencing.

**Tech Stack:** C++23, portable byte types, GoogleTest/Google Mock, Bazel 9.1.1, Gazelle, prek and clang-tidy.

**Spec:** [Confirmed duplication investigation](../specs/2026-10-14-flash-duplication-investigation.md), specifically module responsibilities and proposed sequence; [P79 inventory](../specs/2026-10-14-flash-duplication-triage.md). This is the requested implementation plan for that approved proposal's pure encoding boundary.

## Global constraints

- Keep plans, family state transitions, retries, address translation, erase rules, and result policy explicit in each family.
- Preserve current behavior during extraction; propose evidence-backed corrections separately with a reproducing test and qualification divergence note.
- Expected frames remain independent of shared implementations.
- New helpers are package-owned portable code using bytes and existing injected ports. They add no Qt, owned execution context, filesystem access, public executor hierarchy, or generic family state-machine interface.
- Do not add a new jscpd CI threshold in this proposal.
- Follow the [sharing contract](../../design-notes.md#share-only-proven-protocol-equivalence), [byte conventions](../../coding-style.md#bytes-and-wire-frames) and repository feature-branch workflow. Warnings remain errors; managed BUILD changes come from Gazelle.
- Hardware qualification and address-window guards are unchanged. No protocol correction or external-oracle audit is needed to move this already-identical pure encoder.

## Encoding contract and scope

```cpp
bytes::Bytes DensoIso15765SetupPdu(bytes::Byte service, const MemoryRegion& region);
```

Return exactly eleven bytes: service, `04 44`, four-byte big-endian `region.start`, four-byte big-endian `region.length`. Encode the supplied fields unchanged: no address translation, range checking, addition of start and length, service validation, padding or checksum. Production callers use `0x34` and `0x35`; the existing encoder also passes any other service byte through unchanged.

Use `bytes::ComposeBe` with byte-typed format constants private to the common implementation. Retain `bytes::Bytes` as the output to match current callers. A generic UDS builder or a new module would widen the interface without serving P79.

Source baseline: `7f23ac13`. All four current `SetupPdu` bodies call `ComposeBe(service, kDataFormatIdentifier, kAddressAndLengthFormat, region.start, region.length)` with byte-typed `0x04` and `0x44` constants. Each has three call sites: two in `ReadMemory` and one in `EraseMemory`.

| Family | Current read setup region (start / length) | Policy to preserve |
| --- | --- | --- |
| N83M 1.5M | `0x08FAC000 / 0x00173F00` | Fatal download/upload setup, short timeout |
| N83M 4M | `0x08FAC000 / 0x003D3F00` | Existing tolerant download/upload setup |
| SH72531 | `0x00008000 / 0x00137F00` | Fatal download/upload setup, long timeout |
| SH72543 diesel | `0x00008000 / 0x001F7F00` | Fatal download/upload setup, short timeout |

The inventory's broad “SH long timeout” wording must not be applied to diesel's dump setup. Current source and its `ObkProbeAndReadSetupReadWithTheShortTimeout` regression establish the narrower distinction above. Do not reconcile unrelated protocol questions in this extraction.

## Review focus

Each condition below has coverage assigned to Task 1 or preserved in Task 2.

1. Nonzero high bytes in both fields must survive without truncation or reversed byte order.
2. Zero start and length must still produce all eleven bytes.
3. Maximum 32-bit fields must encode independently, without overflow checks or arithmetic.
4. Upload and download differ only in their service byte; other supplied service bytes also pass through.
5. Shared encoding must not unify 4M tolerance or the differing setup timeouts, nor change erase setup's region.

## File map

All source paths below are under `src/backend/flash/ecu/`:

- Modify `denso_iso15765_can_common.h` and `.cpp`: public pure encoder, private format constants, accurate responsibility comments.
- Modify `denso_iso15765_can_common_test.cpp`: independent literal encoding vectors.
- Modify `subaru_denso_1n83m_1_5m_can_executor.cpp`, `subaru_denso_1n83m_4m_can_executor.cpp`, `subaru_denso_sh72531_can_executor.cpp`, `subaru_denso_sh72543_can_diesel_executor.cpp`: replace twelve calls and remove four local encoders and their now-unused format constants.
- Preserve each corresponding `_executor_test.cpp` as an independent transcript oracle. Change tests only if a demonstrated coverage gap requires it; never build expected setup frames with the new helper.
- Regenerate `BUILD.bazel`; review and keep only necessary generated changes. Existing consumers already depend on the common module.
- Update `docs/design-notes.md` and the P79 inventory row to record the completed boundary when implementation passes verification. Keep the inventory's baseline spans clearly historical.

## Task 1: Add and prove the pure encoder

**Interfaces:** Produces `DensoIso15765SetupPdu` with the signature and exact eleven-byte contract above. Consumes existing `MemoryRegion` (`std::uint32_t start`, `std::uint32_t length`) and `bytes::ComposeBe`; add a direct `flash_types.h` include to the public header for its public type if required by include ownership.

- [ ] Add GoogleTest cases in `denso_iso15765_can_common_test.cpp` with literal byte expectations. Follow the current matcher style; do not generate expected frames with composition helpers.

  | Test name | Service; start; length | Expected bytes |
  | --- | --- | --- |
  | `SetupPduEncodesDownloadBigEndian` | `34; 01234567; 89ABCDEF` | `34 04 44 01 23 45 67 89 AB CD EF` |
  | `SetupPduEncodesUploadBigEndian` | `35; 01234567; 89ABCDEF` | `35 04 44 01 23 45 67 89 AB CD EF` |
  | `SetupPduEncodesZeroFields` | `34; 00000000; 00000000` | `34 04 44 00 00 00 00 00 00 00 00` |
  | `SetupPduEncodesMaximumFields` | `35; FFFFFFFF; FFFFFFFF` | `35 04 44 FF FF FF FF FF FF FF FF` |
  | `SetupPduPreservesServiceByte` | `A5; 00000001; 00000100` | `A5 04 44 00 00 00 01 00 00 01 00` |

  Values in this table are hexadecimal. Each case compares the whole returned vector, pinning length and field order as well as values.
- [ ] Run `bazel test --config=release //src/backend/flash/ecu:denso_iso15765_can_common_test`. Expect compilation failure identifying the missing new operation; investigate unrelated failures before proceeding.
- [ ] Declare and define the encoder in the common header/implementation. Use the unchanged composition expression with private byte-typed format constants. Add a concise comment naming the four bootloader consumers and documenting that this is encoding only.
- [ ] Regenerate with `python3 scripts/gazelle_check.py --fix`, review the generated diff and rerun the focused test. Expect PASS.
- [ ] Stage named Task 1 paths and commit as `refactor(flash): add characterized Denso setup PDU encoder`.

## Task 2: Migrate four families and record the boundary

**Interfaces:** Consumes Task 1's encoder; produces twelve direct family calls with the same service and `MemoryRegion` arguments. No new family interfaces.

- [ ] Before migration, run the focused command below to establish that all four existing family transcript suites pass. They already cover literal download/upload and erase setup frames, 4M malformed/missing-reply tolerance, fatal setup rejection in the other families, and diesel's short setup timeout.
- [ ] Replace each local `SetupPdu(...)` use with `DensoIso15765SetupPdu(...)`; remove the local functions and now-unused format constants. Preserve relevant legacy provenance as concise common-module/call-site comments. Keep every query wrapper, policy, expected response, region argument, log and ordering unchanged. Review exactly three migrated calls per family.
- [ ] Update the common header's responsibility summary and erase comment, plus the owning paragraph in `docs/design-notes.md`: add pure setup-PDU encoding to existing shared responsibilities, leaving caller policy explicit. Update only P79's status in the inventory; P82 is related but its address-translation questions remain unresolved.
- [ ] Run Gazelle regeneration, review `BUILD.bazel`, then rerun the same focused tests. Expect all five targets PASS:

  ```sh
  bazel test --config=release \
    //src/backend/flash/ecu:denso_iso15765_can_common_test \
    //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test \
    //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test \
    //src/backend/flash/ecu:subaru_denso_sh72531_can_executor_test \
    //src/backend/flash/ecu:subaru_denso_sh72543_can_diesel_executor_test
  ```

- [ ] Review the diff against the twelve original call sites. Confirm no family tests import or call the new encoder to generate expectations. Temporarily change common format byte `0x44` to `0x43`; rerun the focused command and require the new unit cases and each family's transcript suite to fail on the wrong frame. Restore the byte and rerun to PASS. This checks that all migrated consumers remain covered independently.
- [ ] Run applicable full gates: `bazel build --config=release //:fastecu`, `bazel test --config=release //...`, `prek run --all-files`, `python3 scripts/gazelle_check.py --fix`, and `bazel run //:clang_tidy_report_changed`. Review any generated changes and rerun affected checks if they change source. Report host/platform limits and actual results; do not claim hardware qualification.
- [ ] Rerun the identical production jscpd scan from the investigation if jscpd 4.0.8 is available; report it as measurement only. Do not add a product dependency or CI threshold for this. Missing scanner tooling does not replace or block correctness gates.
- [ ] Review against the spec and global constraints. Stage named Task 2 paths and commit as `refactor(flash): share setup PDU encoding across Denso families`. Deliver through the feature-branch/PR workflow; push only when authorized.

## Delivery and artifact lifecycle

Planning-only validation is `prek run lychee --all-files` plus scoped diff review; no application build is needed for this document. Implementation must preserve this original plan in delivered Git history before retiring it in a subsequent change, as required by the [documentation lifecycle](../../README.md#recover-completed-work-and-deleted-source-citations). Keep enduring contract knowledge in design notes and unresolved work in its owning inventory.
