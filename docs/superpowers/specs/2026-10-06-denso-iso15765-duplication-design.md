# Share proven Wave 4 Denso ISO-15765 flows

Date: 2026-10-06. Status: proposed design; implementation has not started.

## Goal and evidence

Reduce duplicated protocol code in the [SonarCloud measure for
`subaru_denso_1n83m_1_5m_can_executor.cpp`](https://sonarcloud.io/component_measures?id=RcusStackwalker_FastECU&metric=duplicated_lines_density&view=list)
by factoring only flows whose wire behavior was compared across the relevant
executors. Preserve the established portable backend boundary and the
independent, literal-wire expectations in each executor test suite.

The audit compared the four Wave 4 ISO-15765 executors:

- [N83M 1.5M](../../../src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.cpp)
- [N83M 4M](../../../src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.cpp)
- [SH72531](../../../src/backend/flash/ecu/subaru_denso_sh72531_can_executor.cpp)
- [SH72543 diesel](../../../src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.cpp)

Their seed/key exchange and erase flow have matching request validation,
sequencing, and failure behavior. The seed/key read timeout differed: the first
three use 200 ms and SH72543 diesel uses 2,000 ms. Erase differs in one initial
progress message. The N83M 1.5M and 4M in-car arms also have the same ten
fire-and-forget records and send loop. The SH72531 and SH72543 diesel records
differ, so they are outside that table's scope.

The SonarCloud page identified the target file, but the audit could not retrieve
its exact clone-block ranges. Treat this as a source-audit design; a successful
Sonar analysis after implementation must confirm which findings and density
changed.

## Design

Extend the existing
[Denso ISO-15765 common module](../../../src/backend/flash/ecu/denso_iso15765_can_common.h)
with three narrow shared operations:

1. **Seed/key exchange for all four Wave 4 executors.** Move the identical
   SecurityAccess request-seed/read-seed/derive-key/send-key flow into the
   common module. Use a single 2,000 ms `read_timeout` for both seed and key
   exchanges. The seed remains level `0x61`, the key remains level `0x62`, the
   four seed bytes are taken from the same payload offsets, and the existing
   `denso_seed_key` calculation remains the source of the derived key. Keep
   fatal response validation, cancellation propagation, and success/failure
   logging in the shared operation.

   This intentionally changes the normal response wait from 200 ms to 2,000 ms
   for N83M 1.5M, N83M 4M, and SH72531. SH72543 diesel already uses 2,000 ms.
   Set only `uds::ExchangePolicy::read_timeout`; retain the UDS client's
   existing `pending_timeout` and pending-repeat behavior. Do not propagate this
   timeout to probes, checksum verification, memory reads, or other exchanges.

2. **Erase flow for all four Wave 4 executors.** Move the identical setup
   response check, erase trigger, delay, bounded polling loop, and terminal
   success/failure handling into the common module. Have callers pass the
   already-composed RequestDownload setup PDU. This keeps the wider local
   `setup_pdu` helper in each executor for its read/upload paths. Preserve the
   current wire PDU and expected prefix (`20 01 05`), the 500 ms post-trigger
   delay, at most 20 receives with the existing 500 ms receive and retry
   cadence, including the sleep after each unsuccessful receive, the
   `71 01 02` success check, and the rule that polling never resends the erase
   trigger. Use one shared set of progress messages; retain the existing
   common success and failure messages.

3. **N83M in-car fire-and-forget sequence for N83M 1.5M and 4M only.** Move
   their identical ten-record table and ordered send loop into the common
   module, using the existing `fire_and_forget` primitive. Preserve every
   request CAN ID, payload byte, order, and current per-exchange timeout. Each
   reply is still read and discarded without checking its arbitration ID or
   contents. Keep this table private to these two executors; do not apply it to
   the SH72531 or SH72543 diesel in-car sequences.

Keep `write_memory` orchestration local in all four executors. Also keep
`read_memory`, `reflash_block`, connection and probe flows, kernel jumps,
checksum verification, and family-specific response policies local. Their
similar structure does not establish interchangeable behavior. Do not add
configuration switches or a general-purpose family state machine to the common
module.

## Verification and acceptance

- Add or update focused tests in `denso_iso15765_can_common_test` and all four
  Wave 4 executor suites. Tests must pin the seed/key request and response
  bytes, the 2,000 ms normal response timeout, erase request/poll ordering and
  bounds, and the two N83M in-car tables with literal expected bytes. Keep the
  executor expectations independent of production table constants.
- Confirm the failure, malformed-response, cancellation, and progress-event
  behavior remains covered. In particular, the erase tests must prove that a
  failed poll does not trigger another erase request.
- Build and test the affected Bazel targets, then run the repository's
  applicable formatting, Gazelle, and static-analysis gates. The relevant
  executor tests are `subaru_denso_1n83m_1_5m_can_executor_test`,
  `subaru_denso_1n83m_4m_can_executor_test`,
  `subaru_denso_sh72531_can_executor_test`, and
  `subaru_denso_sh72543_can_diesel_executor_test`.
- Run a successful Sonar analysis on the resulting revision. Record the
  before/after duplicated-lines density and confirm the target's addressed
  clone groups no longer appear. Do not claim a reduction based on source
  deletion alone.
- Update the rationale in `denso_iso15765_can_common.h` and the flash section
  of [design notes](../../design-notes.md) to describe these narrowly shared
  operations and the retained local flows. Update the
  [qualification matrix](../../flash-qualification-matrix.md) to record that
  the 2,000 ms seed/key timeout is a deliberate behavior change for three
  families. Keep all four families' hardware status unchanged until their
  existing qualification evidence supports a change; automated tests alone do
  not establish hardware qualification.

## Risks and boundaries

The longer response window can make a missing seed or key reply take longer to
fail on three families. It does not change request bytes or the UDS pending
timeout. The shared erase loop is write-critical, so its timing and no-resend
behavior must remain pinned by executor-level tests. The in-car table's replies
are intentionally ignored to match current behavior; this work must not
reinterpret them as acknowledgements.

This is a documentation proposal only. No executor, common module, test,
qualification record, or Sonar issue has been changed or marked complete.
