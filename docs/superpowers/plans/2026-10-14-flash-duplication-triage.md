# Flash Duplication Triage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish a staged disposition for all 89 production pairs and deliver the first independently testable extraction: Hitachi/Mitsubishi M32R K-Line optional exchange and Hitachi prefix checking.

**Architecture:** Keep family executors as orchestration owners. Add one package-owned M32R SSM exchange module with explicit addressing and timeout, leaving required-response diagnostics and TCU block pacing local. Independent protocol boundaries require separate follow-up plans; the staging map below is scheduling, not executable instructions for those modules.

**Tech Stack:** C++23, portable backend ports, GoogleTest/Google Mock, Bazel, Gazelle, prek.

**Spec:** [Flash production clone inventory](../specs/2026-10-14-flash-duplication-triage.md), interpreted with the confirmed [investigation design](../specs/2026-10-14-flash-duplication-investigation.md).

## Global Constraints

- “Keep family executors as orchestration owners.”
- “Scanner coordinates are navigation hints, not extraction boundaries.”
- “Use the named enclosing responsibility and inspect source before editing.”
- “Do not strengthen frame validation incidentally.”
- “Keep comment-only, API gate and safety guard clones as intentional.”
- “Existing qualification records explicitly do not establish hardware proof.”
- “No new abstraction; preserve local timeout/rejection policy.”
- Preserve bytes, timing, cancellation checkpoints, errors, logs, retries, address windows and result policy; any correction is separately reviewed under the [correction contract](../../design-notes.md#corrections-require-local-evidence).
- Portable code uses existing injected ports: no Qt, owned threads, direct filesystem access, or new ErrorKind. Production Result/Status checks use `.has_value()`.
- Independent family tests must not compute expected bytes through the new helper. Preserve layer visibility and warnings-as-errors; regenerate managed BUILD files.
- Use a feature branch. Preserve unrelated untracked plans/specs; stage named paths. This planning request does not authorize implementation or push.

## Review Focus

- Absent reply versus engaged empty bytes: optional exchange preserves both; required wrappers map only absence to their existing ECU/TCU timeout detail (Tasks 1–2).
- Transport failure coincident with cancellation: write/read errors retain precedence over the subsequent cancellation check (Task 1).
- Non-default addressing and a 256-byte payload: preserve AddHeader bytes and payload+5 write accounting without adding a new length guard (Task 1).
- Truncated/prefix-mismatched responses, including an empty prefix: preserve offset-four checking and family diagnostics without validating checksum/header (Task 3).
- TCU block exchanges: preserve 100ms before and after the 500ms read, five attempts, and cancellation before the trailing sleep (Task 2).

---

## Scope and staged coverage

The inventory spans independent subsystems. Split follow-up implementation planning by the boundaries below, rather than committing all families to one general interface. Every pair appears exactly once here; action/evidence detail remains owned by the linked spec.

| Stage / separate plan boundary | Pair IDs | Deliverable / prerequisite |
| --- | --- | --- |
| First executable slice: M32R SSM | P06, P14, P16, P17, P18 | Tasks 1–3. P06 paced block path stays local; P17 required wrapper stays local. |
| Unisia SSM | P03, P04, P05 | Separate send/receive helper plan; preserve missing after-write checkpoint and lifecycle cleanup. P05 retained. |
| Mitsubishi CVT pure crypto | P07 | Extend existing `subaru_tcu_cvt_mitsu_can_common`; independent fixed vectors before migration. |
| Pure plan checks | P32, P33, P34, P35, P36, P37, P38, P39, P42 | Extend `plan_primitives`; preserve 6/128-byte upload units, capability precedence, geometry and confirmations. P37 routing retained. |
| Denso K-Line operations | P40, P44, P45, P46, P52, P53, P54, P55, P57 | Separate framing/checked-write/optional exchange plan first; comparison/voltage operations need characterization before their own plan. P52 encoding only; P55 diagnostics local; P57 returned mismatch status unchanged. |
| Denso ISO-15765 pure encoding | P79, P81, P82 | Plan setup PDU first; page operation separately after independent full-page/decrypt/progress fixtures. Preserve page pacing and addressed image construction. |
| Reference-source audit | P11, P12, P19, P22, P23, P24, P25, P26, P48, P49, P50, P51, P56, P61, P62, P63, P64, P65, P66, P67, P71, P73, P74, P75, P76, P77, P78, P80, P83, P84, P85 | Record mapped applicable oracle evidence before planning any normalization or broader shared operation. P25 stop may be planned once return/log/swallowed-error behavior is characterized; existing sharing contract must be reconciled explicitly if that boundary grows. |
| Retain orchestration, interfaces, guards and data | P01, P02, P08, P09, P10, P13, P15, P20, P21, P27, P28, P29, P30, P31, P41, P43, P47, P58, P59, P60, P68, P69, P70, P72, P86, P87, P88, P89 | No extraction in this programme without a new justified seam. P02 full-table alias and P89 chunk planning are optional low-benefit candidates, not scheduled changes. |

Additional inspected candidate, outside the 89-pair count: BEEF CAN parsing for SH7058 petrol/diesel. Give it a separate pure-parser plan with malformed-envelope fixtures and reconciliation of `denso_beef_can_common.h`'s historical two-consumer exclusion. CAN and K-Line envelopes stay separate.

For the audit stage, use local `~/claude-hobby/externals/RomRaider/` and `~/claude-hobby/ecuflash-disassembly/`. Record revision/binary hash, source or containing function, family-selection/caller trace, operation, and observed bytes/timing/error behavior in a focused `docs/reference/flash-duplication-wire-evidence.md` reference. Link it from the existing sharing contract once it contains evidence. The known binary SHA-256 is `db807afd2c9bac193c50f8333baa422097200b54c74c1dea285f0aa40163afa4`; verify identity before using it. RomRaider SSM framing is not an oracle for flash timing. Missing/conflicting evidence means unresolved, preserving current behavior. P80's reversed endpoint is invalid; locate named ReadMemory instead.

Audit completion produces a separately reviewable follow-up spec/plan per proven boundary or defect, with reproducing tests and a qualification divergence note for corrections. It does not authorize executing speculative interfaces. No duplication threshold or new scanner CI dependency.

## File structure for the executable slice

All C++ paths below are under `src/backend/flash/ecu/`:

- Create `m32r_ssm_exchange.h` / `.cpp`: optional framed exchange and pure payload-prefix predicate; names explicitly limit consumers to this proven boundary.
- Create `m32r_ssm_exchange_test.cpp`: package-owned portable tests using existing scripted transport and test-local fault-injecting subclasses.
- Modify `subaru_hitachi_m32r_kline_executor.cpp`, `subaru_tcu_hitachi_m32r_kline_executor.cpp`, `subaru_mitsu_m32r_kline_executor.cpp`: delegate ordinary optional exchange; retain family wrappers, plans and block flow.
- Modify corresponding three `_executor_test.cpp` files: independently specified family errors, addressing and cancellation expectations.
- Regenerate `BUILD.bazel`: helper library/test and consumer implementation dependencies; use existing layer groups.
- Modify `docs/design-notes.md`: name the proven operation and retained boundaries in the owning sharing contract.

No new shared required-response adapter: local wrappers preserve family-specific timeout copy and cost little. No shared send leg for P06 in this slice: keeping the whole paced exchange visible avoids expanding the seam into a policy engine.

### Task 1: Characterize and implement the optional M32R SSM exchange

**Files:** Create the three `m32r_ssm_exchange` files above; regenerate `src/backend/flash/ecu/BUILD.bazel`.

**Interfaces:**
- Consumes `IKlineFlashTransport::Write(bytes::ByteView)`, `Read(std::chrono::milliseconds, const ICancellationToken&)`, and `ssm_protocol::AddHeader`.
- Produces, in `fastecu::flash`:

```cpp
Result<std::optional<bytes::Bytes>> ExchangeM32rSsmOptional(
    IKlineFlashTransport& transport, const ICancellationToken& cancellation,
    bytes::ByteView payload, bytes::Byte tester_id, bytes::Byte target_id,
    std::chrono::milliseconds timeout);
```

- [ ] **Step 1: Add failing GoogleTest cases** in `m32r_ssm_exchange_test.cpp`. Use `ScriptedKlineFlashTransport` initially open and a test-local derived transport overriding Write/Read for short writes, injected faults and cancellation. Record read timeout and write/read call counts. Do not change the reusable mock for these one-suite needs. Name tests and assert the following exact contracts using Result matchers:

| Test name | Assertions |
| --- | --- |
| `WritesExactFrameAndUsesExplicitTimeout` | Payload `{0xbf}`, IDs `0xf0/0x10`, expected write `{0x80,0x10,0xf0,0x01,0xbf,0x40}`; timeout `2000ms`; returned reply exactly `{0x01,0x02}`; script consumed. |
| `PreservesNonDefaultIds` | Payload `{0xbf}`, IDs `0xf1/0x18`, expected write `{0x80,0x18,0xf1,0x01,0xbf,0x49}`; timeout `500ms`. |
| `PreservesAbsentAndPresentEmptyReplies` | No frame returns successful disengaged optional; zero-length frame returns successful engaged empty Bytes. |
| `PreservesLengthWrapWithoutChangingWriteSize` | 256 zero payload bytes produce 261 bytes: prefix `{0x80,0x10,0xf0,0x00}`, 256 zero bytes, checksum `0x80`; accepted only when Write returns 261. |
| `RejectsShortWriteBeforeRead` | Return request-size minus one: `kDisconnected`, detail `short K-Line write`, read count zero. |
| `CancelsBeforeWrite` | Initially cancelled: `kCancelled`, detail `cancelled before write`, no I/O. |
| `CancelsAfterWrite` | Write succeeds then cancels: `kCancelled`, detail `cancelled after write`, one write, no read. |
| `CancelsAfterRead` | Read succeeds then cancels: `kCancelled`, detail `cancelled after read`, one write/read. |
| `WriteFailureWinsOverCancellation` | Write cancels and returns `kDisconnected` / `write fault`; exact error propagated, no read. |
| `ReadFailureWinsOverCancellation` | Read cancels and returns `kDisconnected` / `read fault`; exact error propagated. |

The length-wrap case characterizes existing behavior; correcting it would be separate work. The helper does not parse replies or own a clock.

- [ ] **Step 2: Generate targets and run the new test**: `python3 scripts/gazelle_check.py --fix`, then `bazel test --config=release //src/backend/flash/ecu:m32r_ssm_exchange_test`. Expected initial failure: missing helper implementation/declaration, not an unrelated toolchain failure.
- [ ] **Step 3: Implement the declared signature.** Copy the complete Hitachi optional-exchange semantics: cancel before framing/write; propagate write error; compare actual written count to framed size; cancel after write; read explicit timeout; propagate read error; cancel after read; return optional unchanged. Header documents the three consumers, equivalence and lack of frame validation. No defaults or behavioral switches.
- [ ] **Step 4: Run the same focused test.** Expected all named cases pass. Remove each cancellation check temporarily in turn and confirm its corresponding test fails; restore before proceeding. Confirm error-precedence tests fail if checks move ahead of transport error propagation.
- [ ] **Step 5: Commit the helper and generated graph**, staging only the three new files and `src/backend/flash/ecu/BUILD.bazel`, with message `refactor: add characterized M32R SSM exchange primitive`.

### Task 2: Delegate three family optional exchanges

**Files:** Modify the three family executor `.cpp` and `_test.cpp` files listed above; regenerate `BUILD.bazel`; modify `docs/design-notes.md`.

**Interfaces:** Consumes Task 1's exact `ExchangeM32rSsmOptional` signature. Produces no new API; public executor behavior stays unchanged.

- [ ] **Step 1: Add family characterization tests before migration.** In each existing executor suite add `PreservesRequiredResponseTimeoutDetail`: drive its required exchange to absence and assert `kTimeout` with `no response from ECU` for both ECU families or `no response from TCU` for TCU. Add `PreservesPresentEmptyResponseValidation`: engaged empty reply reaches the existing operation parser, with its existing bad-response detail rather than the missing-frame timeout. Add `PreservesExchangeReadFailure`: inject `kDisconnected` / `read fault` on the first ordinary exchange and assert exact propagation with no later command. Expected wire frames are literal bytes; use existing family plan/transcript setup, not the shared helper.
- [ ] **Step 2: Run characterization tests against the existing executors**:

```sh
bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_m32r_kline_executor_test //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_kline_executor_test //src/backend/flash/ecu:subaru_mitsu_m32r_kline_executor_test
```

Expected PASS before refactoring: these are preservation tests, not new behavior. Reuse the TCU tests `PacesEachBlockReadAsWriteThenDelayThenReadThenDelay`, `RetriesABlockWhenAReadProducesNoFrame`, and `StopsPromptlyWhenCancelledMidRead`; they must continue to pin 100ms/500ms timing and five-attempt behavior. Inspect their existing assertions; add explicit duration checks if only step order is pinned.

- [ ] **Step 3: Replace only each local ordinary `ExchangeOptional` body** with delegation supplying `p.tester_id`, `p.target_id`, and `std::chrono::milliseconds{timeout}` (Hitachi ECU/TCU), or `2000ms` (Mitsubishi ECU). Keep local wrappers and signatures; retain TCU `ExchangeBlockRead` and any Framed function still used there. Delete unused Framed functions only after checking callers. Regenerate managed BUILD files and review new implementation dependencies.
- [ ] **Step 4: Update the owning sharing contract** in `docs/design-notes.md#share-only-proven-protocol-equivalence`: name ordinary optional M32R SSM exchange and its three consumers; explain retained required-response diagnostics, TCU block pacing, handshake, retries and address mapping. Link the helper header. Do not change qualification status.
- [ ] **Step 5: Run Task 2's focused command and Task 1's helper test.** Expected PASS, unchanged full-image transcripts, no additional traffic. Temporarily route TCU block exchange through the ordinary helper and confirm the timing test fails; restore immediately.
- [ ] **Step 6: Commit** the six named family files, regenerated BUILD file and design notes with message `refactor: share ordinary M32R SSM exchanges`.

### Task 3: Share Hitachi payload-prefix predicate and finish qualification gates

**Files:** Modify helper `.h`, `.cpp`, `_test.cpp`; Hitachi ECU/TCU executor `.cpp` and `_test.cpp`; design notes as needed. Mitsubishi `ExpectService` stays local because its copy differs.

**Interfaces:** Consumes Task 1's module. Produces in `fastecu::flash`:

```cpp
bool M32rSsmPayloadHasPrefix(bytes::ByteView response, bytes::ByteView prefix);
```

- [ ] **Step 1: Add failing predicate tests** `MatchesAtOffsetFour` (response `{0,0,0,0,0x67,0x01}`, prefix `{0x67,0x01}` => true), `RejectsShortAndMismatchedPrefix` (five-byte response with two-byte prefix => false; `{0,0,0,0,0x66,0x01}` => false), `EmptyPrefixRequiresFourBytes` (three bytes => false, four => true), and `DoesNotValidateEnvelope` (invalid header/checksum with matching payload => true). These assertions pin existing checks, including empty prefixes.
- [ ] **Step 2: Run** `bazel test --config=release //src/backend/flash/ecu:m32r_ssm_exchange_test`. Expected failure at the new undeclared/unimplemented predicate.
- [ ] **Step 3: Implement the declared predicate** with an offset-four bounds check using subtraction after checking `response.size() >= 4`, then a bounded comparison. Adapt only Hitachi ECU/TCU ExpectPrefix: keep local short-response guard and exact `response is too short`; use the predicate for mismatches with exact `wrong response from ECU` or `wrong response from TCU`. Convert initializer_list to ByteView explicitly; no strengthening of framing validation.
- [ ] **Step 4: Add/run family regression assertions** `PreservesPrefixFailureDetails`: at an existing required-prefix exchange, too-short reply yields `kBadResponse` / `response is too short`; long wrong-prefix reply yields `kBadResponse` with the family's exact mismatch detail. Run helper and all three family executor suites; expected PASS. Mutate offset to five temporarily; predicate test must fail, then restore.
- [ ] **Step 5: Run final implementation gates**, resolving findings without suppression or visibility widening:

```sh
python3 scripts/gazelle_check.py --fix
bazel build --config=release //:fastecu
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy_report_changed
```

Expected zero exit statuses and no managed graph drift or changed-code analysis findings. Host configuration follows `.bazelversion` and `.github/workflows/pr.yml`; platform packaging remains subject to platform CI. Record any unavailable platform gate honestly.

- [ ] **Step 6: Re-run the identical production clone scan** from the investigation, using jscpd 4.0.8 in temporary tooling only, cpp+c-header, 10 lines/70 tokens, excluding tests/support. Compare the operation-level result with baseline 89 pairs/1,582 lines/7.10%; no target percentage. Pair order may change; do not reuse baseline IDs as current IDs.
- [ ] **Step 7: Review scoped diff and commit** helper/prefix changes and any owning documentation/managed graph updates using named paths, message `refactor: share Hitachi M32R payload prefix checking`. Report tests and scan measurement without claiming hardware qualification. Preserve active specs/plans until the scoped work is completed and enduring knowledge extracted; original contents must exist in delivered history before later deletion. Do not delete the programme-wide triage while follow-up work remains unfinished.
