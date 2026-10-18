# Colt Guarded-Transport Pilot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move cancellation for the Mitsubishi Colt M32R CAN family into a
guarded transport, and replace its generic tests with wire transcripts and
one set of guarded-transport tests. The wire behavior must not change.

**Architecture:** A test-data text format (wire transcript) gets a parser, a
formatter, a recorder and a replay transport. Transcripts are recorded from
today's Colt executor before any production change. A `GuardedCanFlashTransport`
decorator is inserted by `BoundAttempt` for executors that opt in. Colt opts
in, its own token checks are deleted, and its generic tests are cut. The
transcripts must replay unchanged throughout.

**Tech Stack:** C++23, Bazel, GoogleTest, `@rules_cc//cc/runfiles`.

**Spec:** [size-reduction strategy, phase 3](../specs/2026-10-18-size-reduction-strategy.md#3-transport-level-cancellation-and-the-transcript-harness),
[ADR 0024](../../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md),
[ADR 0025](../../adr/0025-enforce-cancellation-in-a-guarded-transport.md).

## Global Constraints

- Wire behavior is the contract: bytes, ordering, timeouts and delays, retry
  counts, address windows, and which failures abort. Log text, error detail
  strings and progress granularity may change.
- The backend stays Qt-free; new targets are `fastecu_portable_gtest` or
  plain `cc_library` under `src/backend`.
- Naming per [coding style](../../coding-style.md): `CamelCase` functions,
  `lower_case` locals, `kCamelCase` constants and enumerators.
- Pilot scope is CAN only. There is no K-Line decorator and no
  expected-silence API, and the check in `UdsClient::Request` stays (14
  production users are still unguarded).
- Colt declares no critical section. A cancel after the erase request still
  stops before its reply.
- A transcript is regenerated from a known-good implementation, never edited
  by hand. If replay fails after a production change, the production change is
  wrong.
- Before deleting each test, confirm that a named shared suite covers the same
  behavior. If none does, move the test there instead of dropping it.
- Measure NLOC before the first PR and after the last, with
  `python3 scripts/nloc_loop.py measure` on the formatted tree. Record both
  numbers in the spec's phase table.

## Review Focus

1. **A cancel that lands between a request and its reply.** Today it stops
   before the reply (`ACancellationThatArrivesAfterTheRequestStopsBeforeTheReply`).
   The guarded transport must keep that: its `Read` checks the token before it
   delegates. This is pinned in Task 4.
2. **A cancel during a `UdsClient` pre-read delay.** `FakeClock::Sleep` and the
   real clock already return `kCancelled`. The conformance case in Task 5
   cancels during a phase and expects `kCancelled`, never `kTimeout` or
   `kBadResponse`.
3. **Close after cancel.** `BoundAttempt` must still close the transport
   exactly once, and the guarded transport must never refuse `Close`. This is
   pinned in Task 4 (`CloseIsNeverRefused`) and Task 5.
4. **A declined confirmation is reported as cancellation.** The erase trigger
   and the top-region bootstrap still return `kCancelled` from the executor
   when a confirmation is absent. The existing tests stay (Task 6 keep list).
5. **Ambiguous payload references.** If an image run repeats, the formatter
   could reference the wrong offset. The replay still compares bytes, so a
   wrong but equal reference is harmless. A reference past the end of the image
   must fail to parse (Task 1).

---

### Task 1: Wire transcript format

**Files:**
- Create: `src/backend/flash/testing/wire_transcript.h`, `wire_transcript.cpp`
- Test: `src/backend/flash/testing/wire_transcript_test.cpp`
- Modify: `src/backend/flash/testing/BUILD.bazel` (testonly `cc_library` `wire_transcript`, plus its test)

**Interfaces:**
- Produces:
  ```cpp
  namespace fastecu::flash {
  struct WireTranscriptStep {
      enum class Kind { kWrite, kRead, kNoFrame, kReadTimeout, kSleep, kSection };
      Kind kind;
      bytes::Bytes frame;                    // kWrite, kRead
      std::chrono::milliseconds duration{};  // kReadTimeout, kSleep
      std::string label;                     // kSection
      bool operator==(const WireTranscriptStep&) const = default;
  };
  using WireTranscript = std::vector<WireTranscriptStep>;
  Result<WireTranscript> ParseWireTranscript(std::string_view text, bytes::ByteView image);
  std::string FormatWireTranscript(const WireTranscript& transcript, bytes::ByteView image);
  }
  ```

Grammar, one step per line:
- `> <hex bytes and refs>` is a write, and `< <hex bytes and refs>` is a read.
- `< none` is a read that got no frame.
- `@ read-timeout <ms>` and `@ sleep <ms>` are timing steps.
- `# section <label>` labels the steps that follow.
- Blank lines and `## <comment>` lines are ignored.

Hex is space-separated lowercase pairs. A ref is either `{image 0x<off>+<len>}`
(decimal `len`) or `{fill <hh> <len>}`. Parsing expands refs into `frame`.

`FormatWireTranscript` emits one step per line. Within each frame it replaces:
- any run of 32 or more identical bytes with `{fill}`;
- otherwise, the longest suffix of 32 or more bytes, starting at index 1 or
  later, that occurs in `image`, with `{image}`. Find it with
  `std::boyer_moore_horspool_searcher`, and take the first occurrence at or
  after the previous reference's end, so sequential chunks reference
  sequential offsets.

- [ ] **Step 1: Write the failing tests** in `wire_transcript_test.cpp`:
  - `ParsesEachStepKind`: text `"# section connect\n> 00 00 07 e0 10 85\n@ read-timeout 1000\n< 00 00 07 e8 50 85\n< none\n@ sleep 20\n"` parses to six steps with those kinds, frames, durations and the label `"connect"`.
  - `ExpandsImageAndFillReferences`: with `image` = bytes `0..255`, the text `"< 63 {image 0x10+4} {fill ee 3}"` gives frame `{0x63,0x10,0x11,0x12,0x13,0xee,0xee,0xee}`.
  - `RejectsAReferencePastTheImage`: `"> {image 0xfe+4}"` against a 256-byte image is `IsErr(ErrorKind::kInvalidConfig)`.
  - `RejectsAnUnknownLine`: `"? 00"` is `IsErr(ErrorKind::kInvalidConfig)`, and the error detail contains `"line 1"`.
  - `FormatRoundTrips`: take a transcript of a 64-byte write that copies `image[0x40..0x80)` after a 5-byte header, plus a 40-byte `0xff` read. Formatting it yields one `{image 0x40+64}` and one `{fill ff 40}`, and parsing the result gives back an equal transcript.
- [ ] **Step 2: Run them and confirm they fail to build.** Run `bazel test //src/backend/flash/testing:wire_transcript_test`.
- [ ] **Step 3: Implement** `ParseWireTranscript` and `FormatWireTranscript` with the signatures above. Errors are `Fail(ErrorKind::kInvalidConfig, std::format("line {}: ...", n))`.
- [ ] **Step 4: Run the test** and confirm it PASSES.
- [ ] **Step 5: Commit:** `test: add the wire transcript format`.

### Task 2: Transcript recorder and replay transport

**Files:**
- Create: `src/backend/flash/testing/transcript_can_flash_transport.h` (header-only, like the scripted transports)
- Modify: `src/backend/flash/testing/scripted_can_flash_transport.h` (add `const std::vector<std::string>& Sections() const { return sections_; }`)
- Test: `src/backend/flash/testing/transcript_can_flash_transport_test.cpp`
- Modify: `src/backend/flash/testing/BUILD.bazel`

**Interfaces:**
- Consumes: `WireTranscript`, `WireTranscriptStep` (Task 1); `ScriptedCanFlashTransport::Sections()`.
- Produces:
  ```cpp
  // Wraps a scripted transport and clock; records every Write/Read/Sleep.
  class RecordingTranscriptCanFlashTransport final : public ICanFlashTransport, public IClock {
    public:
      RecordingTranscriptCanFlashTransport(ScriptedCanFlashTransport& inner, IClock& clock);
      const WireTranscript& Transcript() const;
  };
  // Replays a transcript; any deviation fails with kInternal.
  class ReplayCanFlashTransport final : public ICanFlashTransport, public IClock {
    public:
      explicit ReplayCanFlashTransport(WireTranscript transcript);
      bool Consumed() const;
  };
  ```

Recorder behavior:
- It emits `kSection` when the inner script's section for the next write
  differs from the last one it emitted.
- It emits `kReadTimeout` only when the timeout differs from the previous read's.
- A successful `Read` returning a frame becomes `kRead`. One returning
  `std::nullopt` becomes `kNoFrame`.
- Lifecycle calls (`ResetConnection`/`Configure`/`Open`/`Close`) are
  forwarded and not recorded.

Replay behavior:
- `Write` must match the next non-section step as a `kWrite` with an equal frame.
- `Read` checks its timeout against the last `kReadTimeout`, then consumes a
  `kRead` or `kNoFrame`.
- `Sleep` must match a `kSleep` of equal duration and then honors the token as
  `FakeClock` does.
- Lifecycle calls succeed and are not checked.
- Every mismatch is `Fail(kInternal, ...)`, naming the step index, the current
  section and both values.

- [ ] **Step 1: Write the failing tests.**
  - `RecordsWritesReadsSilenceTimeoutsAndSections`: script two exchanges in sections `"a"` and `"b"`, the second answered by `QueueNoFrame()`, and drive them through the recorder with 1000 ms and then 500 ms reads. Expect `{kSection a, kWrite, kReadTimeout 1000, kRead, kSection b, kWrite, kReadTimeout 500, kNoFrame}`.
  - `ReplayAcceptsTheRecordedRun`: feeding that transcript to `ReplayCanFlashTransport` and repeating the same calls succeeds, and `Consumed()` is true.
  - `ReplayRejectsADivergentWrite`: a write with one byte flipped is `IsErr(kInternal)`, and the detail contains `"step 1"` and `"section a"`.
  - `ReplayRejectsAChangedReadTimeout`: reading with 999 ms where 1000 was recorded is `IsErr(kInternal)`.
  - `ReplayRejectsAnUnrecordedSleep`: `Sleep(20ms)` with no `kSleep` step next is `IsErr(kInternal)`.
- [ ] **Step 2: Run them and confirm they fail.** Run `bazel test //src/backend/flash/testing:transcript_can_flash_transport_test`.
- [ ] **Step 3: Implement both classes** as specified above.
- [ ] **Step 4: Run the test** and confirm it PASSES.
- [ ] **Step 5: Commit:** `test: record and replay CAN wire transcripts`.

### Task 3: Record the Colt transcripts and replay them

**Files:**
- Create: `src/backend/flash/ecu/testing/mitsu_colt_m32r_can_scripts.h` and `.cpp` (testonly). This moves `Request`, `Response`, `ReadPlan`, `WritePlan`, `WriteRom`, `TopRegionOf`, `UserspaceOf`, `ScriptBootloadHandshake`, `ScriptAddressMarkedRead`, `ScriptFlashRead*`, `ScriptUploadAndCommit` and `ScriptUnlockAndErase` out of `mitsu_colt_m32r_can_executor_test.cpp` unchanged, into `namespace fastecu::flash::colt_testing`.
- Create: `src/backend/flash/ecu/testing/record_mitsu_colt_m32r_can_transcripts.cpp` (testonly `cc_binary`)
- Create: `src/backend/flash/ecu/testdata/mitsu_colt_m32r_can/{read_384k,write_384k,write_512k_bootstrap}.transcript`
- Create: `src/backend/flash/ecu/mitsu_colt_m32r_can_transcript_test.cpp`
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp` (use the moved helpers), `src/backend/flash/ecu/BUILD.bazel`, `src/backend/flash/ecu/testing/BUILD.bazel`

**Interfaces:**
- Consumes: Tasks 1–2; `BindFlashAttempt` from `flash_executor.h`.
- Produces: `colt_testing::` helpers with their current signatures;
  `colt_testing::AddressMarkedImage(std::uint32_t length) -> bytes::Bytes`,
  which is the ECU memory `ScriptAddressMarkedRead` serves, extracted from
  that function.

The recorder binary takes `--out <dir>`. It runs three scenarios, each
through `RecordingTranscriptCanFlashTransport` and calling
`executor.Execute(...)` directly exactly as the current tests do, and writes
`FormatWireTranscript(..., image)` into each file:
- `read_384k`: the `ScriptBootloadHandshake` + `ScriptAddressMarkedRead(0x60000)` script with `ReadPlan(kProtocol384)`, against `AddressMarkedImage(0x60000)`.
- `write_384k`: the full userspace write script for `WritePlan(WriteRom())` on `kProtocol384`, against `WriteRom()`.
- `write_512k_bootstrap`: the script of `WriteRunsTheBootstrapWhenTheTopRegionDiffers`, against its 512 KiB ROM.

- [ ] **Step 1: Move the helpers.** Run `bazel test //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test` and confirm it PASSES, with the same 46 cases.
- [ ] **Step 2: Write the recorder** and run `bazel run //src/backend/flash/ecu/testing:record_mitsu_colt_m32r_can_transcripts -- --out "$PWD/src/backend/flash/ecu/testdata/mitsu_colt_m32r_can"`. Expect three files. Each starts with `# section`, has no line longer than 200 characters, and `read_384k` has 2,048 `>` lines.
- [ ] **Step 2b: Inventory deferrals and silence.** In the executor and in
  `UdsClient`/`CanFlashUdsChannel`, list every place that either returns
  success after a no-frame read, or reads the token and then does not stop.
  The expected result is none of either. The recorder writes the result as
  the header lines `## critical sections: none` and `## expected silence: none`
  in each file. If either list is not empty, stop and bring it back to the
  spec before Task 4.
- [ ] **Step 3: Write the replay test** `mitsu_colt_m32r_can_transcript_test.cpp`, with `data = glob(["testdata/mitsu_colt_m32r_can/*.transcript"])` and files loaded through `rules_cc` runfiles. Its helper `ReplayThroughRunner(plan, transcript_name, image)` parses the file, wraps `ReplayCanFlashTransport` in `BindFlashAttempt(plan, std::make_unique<MitsuColtM32rCanExecutor>(), ...)`, and runs it with the replay object as the clock. The cases are:
  - `ReadReplays384KiB`: expect `IsOk()`, `read_bytes == AddressMarkedImage(0x60000)`, and `Consumed()`.
  - `WriteReplays384KiB` and `WriteReplays512KiBBootstrap`: expect `IsOk()` and `Consumed()`.
- [ ] **Step 4: Run** `bazel test //src/backend/flash/ecu:mitsu_colt_m32r_can_transcript_test` and confirm it PASSES on the unmodified executor.
- [ ] **Step 5: Commit** the helpers, the recorder, the transcripts and the replay test: `test: record Colt M32R CAN wire transcripts`. This is the first PR and the equivalence baseline.

### Task 4: Guarded CAN transport

**Files:**
- Create: `src/backend/flash/guarded_can_flash_transport.h` and `.cpp`
- Test: `src/backend/flash/guarded_can_flash_transport_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`

**Interfaces:**
- Produces:
  ```cpp
  namespace fastecu::flash {
  class GuardedCanFlashTransport final : public ICanFlashTransport {
    public:
      GuardedCanFlashTransport(ICanFlashTransport& inner, const ICancellationToken& cancellation);
      // While alive, Write/Read do not stop on cancellation. The first
      // Write/Read after the last section ends reports kCancelled.
      class CriticalSection;
      [[nodiscard]] CriticalSection EnterCriticalSection();
  };
  }
  ```

The decorator behaves as follows:
- `Write` and `Read` return `Fail(ErrorKind::kCancelled, "cancelled")` without
  touching `inner` when the constructor token is cancelled and no critical
  section is open. Otherwise they delegate, forwarding the caller's token.
- Inner errors and `std::nullopt` pass through unchanged.
- `ResetConnection`, `Configure`, `Open`, `Close` and `RequestUnblock` delegate
  unconditionally.
- It does not override `RestartIso15765`, so the default calls the guarded
  lifecycle methods.
- Sections nest, tracked by a depth counter.

- [ ] **Step 1: Write the failing tests** over `ScriptedCanFlashTransport`, using `ManualCancellationToken`:
  - `ACancelledWriteNeverReachesTheTransport`: after cancel, `Write` is `IsErr(kCancelled)` and `WritesConsumed() == 0`.
  - `ACancelBetweenRequestAndReplyStopsBeforeTheReply`: the write succeeds, cancel happens, then `Read` is `IsErr(kCancelled)` and `ReadTimeouts()` is empty.
  - `TransportErrorsPassThroughUnchanged`: for each of `kTimeout`, `kDisconnected` and `kBadResponse` queued via `QueueError`, `Read` returns that kind.
  - `NoFramePassesThrough`: after `QueueNoFrame()`, `Read` is `IsOk()` with `std::nullopt`.
  - `CloseIsNeverRefused`: after cancel, `Close()` is `IsOk()` and `close_call_count == 1`.
  - `ACriticalSectionDefersCancelToTheNextIo`: cancel inside a section, and one exchange still completes. After the section ends, the next `Write` is `IsErr(kCancelled)`.
  - `NestedCriticalSectionsDeferUntilTheOutermostEnds`.
  - `RequestUnblockReachesTheTransport`: `QueueBlockingRead`, then a `Read` on another thread returns once `RequestUnblock()` is called on the guard.
- [ ] **Step 2: Run them and confirm they fail.** Run `bazel test //src/backend/flash:guarded_can_flash_transport_test`.
- [ ] **Step 3: Implement** `GuardedCanFlashTransport` as specified.
- [ ] **Step 4: Run the test** and confirm it PASSES.
- [ ] **Step 5: Commit:** `feat: add the guarded CAN flash transport`.

### Task 5: Run Colt behind the guarded transport

**Files:**
- Modify: `src/backend/flash/flash_executor.h` (`BoundAttempt::Run`)
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h` (opt-in marker)
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_transcript_test.cpp` (conformance case)
- Modify: `src/backend/flash/BUILD.bazel` (`flash_executor` depends on `guarded_can_flash_transport`)

**Interfaces:**
- Consumes: `GuardedCanFlashTransport` (Task 4).
- Produces: `static constexpr bool kRunsBehindGuardedTransport = true;` on `MitsuColtM32rCanExecutor`. In `BoundAttempt::Run`:

  ```cpp
  if constexpr (requires { requires Executor::kRunsBehindGuardedTransport; })
  ```

  When that holds, `Execute` receives a `GuardedCanFlashTransport` built on
  `*transport_` and `cancellation`. Every other executor is unchanged. The
  marker is deleted once phase 4 has migrated every CAN family.

- [ ] **Step 1: Write the failing conformance case** `CancelGoesThroughTheGuardedTransport`. It runs the `read_384k` script through `BindFlashAttempt`, using `PhaseCancellingEventSink` (from `ecu/testing/recording_can_flash_transport.h`), and cancels on entering `"Read ROM"`. The scripted transport is wrapped so that its own token checks see a token that is *never* cancelled. That way only the guard can stop the run. Expect `IsErr(kCancelled)`, `WritesConsumed()` equal to the handshake's exchange count, and `close_call_count == 1`.
- [ ] **Step 2: Run it.** It currently PASSES only by accident, through the executor's own chunk check. Temporarily comment out the check at `mitsu_colt_m32r_can_executor.cpp:245`, confirm the test now FAILS (the read continues), then restore the line.
- [ ] **Step 3: Add the marker and the `BoundAttempt` branch.**
- [ ] **Step 4: Run** `bazel test //src/backend/flash/...`. Expect everything to PASS, including the three transcript replays and the CAN conformance suites of the other families.
- [ ] **Step 5: Commit:** `feat: run the Colt M32R CAN executor behind the guarded transport`.

### Task 6: Delete Colt's own cancellation and its generic tests

**Files:**
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`. Delete the token checks at `:245`, `:287`, `:418` ("cancelled after erase") and `:678` ("cancelled before setup"). `ConfirmationGranted` returns stay.
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`
- Possibly modify: `src/backend/protocol/uds/uds_client_test.cpp`, `src/backend/flash/ecu/uds_client_exchange_common_test.cpp`, `src/backend/flash/can_flash_uds_channel_test.cpp` (receivers of moved cases)
- Modify: `docs/flash-qualification-matrix.md` (Colt row: add `mitsu_colt_m32r_can_transcript_test`)

**Delete**, each only after naming the shared test that covers it in the
commit message, and otherwise moving it into the receiver above that owns the
behavior:
- `ReadReportsAnEmptyReplyAsTimeout`, `ReadPropagatesADisconnectedTransport`, `ReadTimeoutPropagates`
- `ReadStopsWhenCancelled`, `ReadStopsAtTheNextChunkWhenCancelledMidRead`, `WriteStopsAtTheNextExchangeWhenCancelledMidWrite`, `ACancellationThatArrivesAfterTheRequestStopsBeforeTheReply` (all covered by Task 4 and Task 5)
- `ACancelledEraseTriggerIsNotReportedAsAnEcuRejection`, `AFailedTransportWriteIsReportedWithoutWaitingForAReply`
- `WriteStopsWhenTheReflashUnlockIsRejected`, `WriteStopsWhenTheEraseTriggerIsRejected`, `WriteAbortsWhenTheEraseRoutineRequestDownloadIsRejected`, `WriteAbortsWhenTheWriteRoutineTransferDataIsRejected`, `BootstrapAbortsWhenTheChecksumRequestDownloadIsRejected`, `BootstrapAbortsWhenTheChecksumTransferDataIsRejected`, `BootstrapReportsItsOwnReflashUnlockRejection` (shared `FatalQuery`/`FatalRequest` behavior)
- `AbsorbsResponsePendingWithoutResending`, `FailsWhenTheEcuPendsPastTheRepeatLimit`, `RejectsAResponseToADifferentService`, `ReadRejectsAChunkAnsweredWithTheWrongService`, `HandshakeRejectsAReplyTooShortToHoldAServiceByte` (owned by `UdsClient`)
- `ReadReturnsEachProtocolCapacityFromAddressZero`, `WriteDrivesTheBootloadSessionThenFactorySecurityAccess`, `WriteRunsTheBootstrapWhenTheTopRegionDiffers` (superseded by the transcripts. The 512 KiB read window is covered by the plan test.)

**Keep** every other case, including:
- plan rejection and wire parameters
- `WriteBoundsTheDefaultProtocolTo384KiB` and `WriteSkipsBootstrapWhenTheTopRegionAlreadyMatches`
- the carrier-mismatch and carrier-read cases
- `WriteStopsReadingTheTopRegionAtTheFirstMismatchedChunk`
- both verify mismatches and `WriteFailsWhenTheUserspaceCrcCheckIsRejected`
- `WriteStopsWhenTheCarrierEraseTriggerReportsANonZeroStatus` and `MitsuColtM32rCanExecutorRoutineStatusTest`
- both absent-confirmation cases and `RefusesATestWritePlanRatherThanWritingForReal`
- the vendor challenge order and both key-rejection cases
- `RejectsAFrameFromTheWrongReplyId` and `ReadEmitsMonotonicProgress`

- [ ] **Step 1: Delete the listed tests**, moving any that lack shared coverage. Run `bazel test //src/backend/...` and confirm it PASSES.
- [ ] **Step 2: Delete the four token checks.** Run `bazel test //src/backend/flash/...` and confirm that every transcript replays, the conformance case passes, and every kept case passes.
- [ ] **Step 3: Measure** with `python3 scripts/nloc_loop.py measure`. Record production and test NLOC against the Task 3 baseline in the spec's phase table, in the `3.` row's Expected column.
- [ ] **Step 4: Commit:** `refactor: let the guarded transport own Colt M32R CAN cancellation`.

### Task 7: Pilot exit gate

- [ ] **Step 1:** Run `docs/checklists/colt_czt_47110032_can_bench_checklist.md` on the bench ECU with the post-Task 6 build, for both capacities, and include a cancel during the read. Record the results in the checklist.
- [ ] **Step 2:** If the net NLOC change from Task 6 is a reduction, mark phase 3 done in the spec. If not, record why and stop before phase 4.
