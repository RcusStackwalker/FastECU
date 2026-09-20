# Step 5 Tail Wave 6a-3 — Hitachi SH72543R CAN

**Status:** design agreed in conversation; written spec awaiting review.
**Parent:** [wave 6 singletons](2026-09-19-step5-tail-wave6-singletons-design.md).
**Predecessors:** wave 6a-1 (#347) and 6a-2 (#348), both merged.
**Source baseline:** `5dc86672`.

## Intent and success criteria

Migrate `FlashEcuSubaruHitachiSH72543rCan` to a portable plan and executor,
using the existing desktop `FlashWorkflow`. Support both configured normal
and recovery protocol names, remove this family's legacy operation and dialog,
and reduce the legacy drain from eight families to seven. Keep hardware status
experimental. This is one family migration and one implementation PR.

The user approved rejecting `test_write` before I/O and explicitly documenting
narrowly justified corrections to defective legacy behavior. This family-specific
policy supersedes the parent spec's blanket behavior-preservation rule. Other
wire behavior remains unchanged. The user also approved the architecture and
error-handling/test contract below; approval of this written artifact is separate.

## Approach and scope

Use the same component boundaries as #348: family types, pure plan construction
and validation, a synchronous cancellable `ICanFlashExecutor`, and a desktop
workflow that binds the executor to `DesktopCanFlashTransport` and `QtClock`.
Use the shared flash dialog/worker flow rather than rewriting the old family
dialog around a worker. Backend code must not depend on Qt, `SerialPortActions`,
or `EcuCalDefStructure`.

Combining the SH7058 migration would enlarge review scope. Extracting a shared
Hitachi executor first would assume protocol equivalence that has not been
established. Neither is in scope. No new transport surface, kernel upload,
shared protocol state machine, protocol-catalog change, or hardware qualification
is included. If an actual transport gap is found, stop and revise the design.

## Plan contract

Add `SubaruHitachiSh72543rCanPlan` and the corresponding family/variant entry,
following the current family naming and header organization. Provide
`build_subaru_hitachi_sh72543r_can_plan` and
`validate_subaru_hitachi_sh72543r_can_plan`.

Accept exactly `sub_ecu_hitachi_sh72543r_can` and
`sub_ecu_hitachi_sh72543r_can_recovery`, both bound to MCU `SH72543R`.
Recovery is an alias for the same sequence: legacy does not branch on the name.
Read and Write are supported; TestWrite returns Unsupported. Reject other
protocol/MCU combinations, incorrect image sizes, and inconsistent family,
region, kernel, or operation fields before I/O. The builder and validator enforce
the contract; executor setup and execution consume validation so a manually
constructed plan cannot bypass it.

- Read window: address zero, length `0x200000`, pages of `0x400` bytes.
  Successful output is exactly 2 MiB, with no decryption or synthetic padding.
- Write input: exactly `0x200000` bytes, indexed from address zero.
  Program block 1 only, `[0x6000, 0x200000)`, in `0x100`-byte frames.
  Block 0 is never transmitted for programming. The advertised download window
  is start `0x6000`, length `0x1FA000`, matching `fblocks_SH72543R`.
- No kernel input: legacy only logs its kernel field and performs no upload.
- ISO-15765, 500 kbit/s, 11-bit request/response IDs `0x7E0`/`0x7E8`.
  Assert every remaining configuration field against the existing
  `configureIso15765Can` helper rather than relying on defaults.

Not transmitting block 0 does not prove the ECU's erase command preserves it;
physical erase scope remains a bench qualification question.

## Executor and data flow

Keep family constants and protocol helpers local. Reuse portable SSM crypto
primitives with this family's tables; do not copy the TCU family's keys or
assume that the unused legacy decrypt helper belongs in the read path.

Connection first sends the `B7` probe. The legacy `7F B7 13` signature selects
the already-running-kernel shortcut. Otherwise query ECU ID (`AA`), VIN
(`09 02`), CAL ID (`09 04`), and CVN (`09 06`), followed by the two legacy
`A8` initialization requests. Identification is optional; parse only complete
fields and preserve the CAL-ID/ECU-ID ROM-name composition when available.
Return metadata through the existing executor result rather than mutating a
calibration structure. A shortcut with no queried identity leaves metadata absent.

Read enters session `10 03`, requests seed `27 01`, sends key `27 02`, and
issues 2,048 `23 24` page requests. Each page must have service `63` and exactly
`0x400` data bytes after the four-byte CAN prefix and service byte. Append only
validated pages. Finish with the legacy `10 01` stop sequence, up to six sends,
whose missing or wrong reply remains nonfatal. Publish read bytes only after a
successful complete transfer; cancellation/failure does not publish partial data.

Write encrypts the input using the family's existing payload transform. Enter
`10 43`, perform seed/key access, jump with `10 42`, set the download window
with `34 04 33 00 60 00 1F A0 00`, then send the existing erase request
`31 01 02 01 0F FF FF FF`. After confirmed erase, send 8,096 `B6` frames,
using encrypted image bytes at each frame's absolute address. Close with `37`
and verify with `31 01 02 02 01`. Both close and checksum retain up to 20
request attempts and the legacy positive signatures.

Every exchange receives a comment citing the legacy file and line at the source
baseline. Preserve response timing: 200 ms for the initial probe read,
2,000 ms for ordinary reads and checksum replies, 800 ms for read-stop and
write-close replies. Preserve the call-site delays (including 10 ms before
reading each data-frame reply) rather than importing the TCU defaults.

## Error handling and explicit corrections

1. **Unsupported dry run:** reject TestWrite before any transport operation.
   Legacy passes a flag into `reflash_block` but never consumes it.
2. **Bounds-safe parsing:** require a full four-byte seed before extraction.
   Incomplete optional identification is logged/skipped; an incomplete required
   response fails as BadResponse. Never reproduce out-of-bounds accesses.
3. **Read integrity:** require the positive page service and exact page size.
   Legacy strips five bytes from any nonempty-looking reply and advances even
   when no page arrived, allowing success with a corrupt/truncated image.
4. **Complete write frames:** allocate or append the complete header and payload.
   Legacy indexes beyond an eight-byte QByteArray while constructing the payload.
5. **Cancellation:** return Cancelled, never success. Check before transport
   operations, between pages/frames, and within bounded retry/delay sequences.
   Stop subsequent programming commands when cancellation is observed; do not
   invent a recovery or rollback transaction.
6. **First erase response:** inspect the initial read instead of discarding it.
   Accept `71 01 02` immediately. Otherwise retain at most 20 subsequent reads,
   with the existing 500 ms delay after each unsuccessful polling response.
   Nonmatching responses and timeouts consume this bounded polling budget;
   transport failure or cancellation terminates it. Exhaustion fails the attempt
   and no B6 frame follows. No automatic erase retransmission is introduced.
7. **Transport failures:** propagate failed writes and disconnects rather than
   ignoring them. Optional response timeouts remain optional; required single
   exchanges return Timeout, and retrying exchanges consume their bounded budget.
8. **Defined progress/log calculations:** initialize timing/rate values before
   use and keep progress ordered through the existing phase-progress pattern.
   Preserve meaningful log text, not undefined numeric output or per-block
   success after cancellation.

Preserve tolerated `10 03`/`10 43` session responses, optional identification,
the ignored content of the two A8 initialization replies, and nonfatal read-stop
responses. Do not apply a generic UDS helper that silently changes these policies
or retransmission timing.

B6 reply contents remain uninterpreted, as in legacy. A missing response is
nonfatal at this exchange; a write failure or disconnect is fatal. The final
close and checksum responses are required. This limitation must be recorded in
the qualification matrix, not presented as per-frame acknowledgement checking.
On exhausted close/checksum retries, report Timeout if no response arrived and
BadResponse if replies arrived but none matched. Apply the same distinction to
erase exhaustion. A mandatory seed/key/session-jump/download reply with wrong
content is BadResponse. Preserve specific transport errors instead of flattening
them into a protocol mismatch.

## Desktop integration and removal

Register both exact protocol aliases with `FlashWorkflow`, using the existing
Begin prompt, bound attempt ownership, outcome handling, read inspection/save,
and failure flow. Pass the current ROM image for Write and optional returned
ROM identity for Read. A rejected plan or declined Begin prompt starts no
attempt. Follow the shared UI route already used by #348.

Delete this family's legacy operation `.h/.cpp`, obsolete dialog `.h/.cpp`,
and their MainWindow dispatch/include references after routing both aliases.
Update relevant Bazel dependencies and tests. Retain shared legacy utilities,
worker, package, and compatibility allowlist until wave 7.

Register every new backend library explicitly in
`bazel/portable_targets.bzl`'s `PORTABLE_PACKAGES`. Remove exactly this family's
entry from `scripts/check-legacy-flash-drain.py`. Update the qualification matrix
with portable=yes, experimental status, test labels, all deliberate corrections,
and preserved limitations. Update wave-6 progress documentation to record
6a-1/6a-2 as merged and 6a-3 when implemented; this spec itself does not claim
that migration has landed.

## Verification contract

Plan tests cover both aliases, exact MCU and configuration, read/write geometry,
image size errors, unsupported operations, unwanted kernels, and forged invalid
plans. Verify invalid executor input performs no I/O.

Executor tests use scripted transport and fake time to pin complete read and
write transcripts, including all page/frame addresses and payload bytes.
Independently anchored crypto vectors and nonuniform images detect wrong tables
or offsets; expected payloads must not simply call the executor's own helper.
Cover the last frame and the absence of programming below `0x6000`.

Pin the probe shortcut and initialization path, optional metadata and truncated
identity replies, short seeds, wrong/short/oversized page replies, timeout and
disconnect, failed writes, immediate/delayed/exhausted erase completion, close
and checksum retry limits, tolerated session replies, ignored B6 response
contents/timeouts, nonfatal read-stop exhaustion, and cancellation in connection,
read, erase polling, write, and finalization. Assert failed erase is followed by
no programming frame and cancellation never produces a completed read/write.

Workflow tests cover dispatch for both aliases, image/metadata flow, declined
confirmation, rejected preflight, read inspection/save/discard, and propagated
attempt failure. Reuse shared dialog tests where behavior is unchanged.

Implementation gates inherited from the drain:

```sh
bazel build -k --config=release //:fastecu //tests/...
bazel test -k --config=release //tests/... //:bazel_openssl_wiring \
  //:serial_compat_allowlist //:portable_closure //:legacy_flash_drain
```

Run relevant package-owned tests explicitly if not included by the aggregate.
Require at least 80% new-code coverage and the SonarCloud Quality Gate for the
implementation PR. This design-only commit requires document/diff checks, not
product builds. Software tests cannot establish physical erase scope, ECU
compatibility, or hardware safety; retain experimental status and record bench
verification of block-0 preservation and programmed-image readback.

## Review and next stage

Written-spec approval permits creating the implementation plan with the
writing-plans skill. It does not authorize skipping plan review or selection of
execution method. No implementation is included in this design task.
