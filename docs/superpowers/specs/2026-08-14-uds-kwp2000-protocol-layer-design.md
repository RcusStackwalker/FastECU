# UDS/KWP2000 protocol layer

Design for [issue #170](https://github.com/RcusStackwalker/FastECU/issues/170),
"Implement UDS/KWP2000 protocol — to be used by loggers/flashers with
vendor-specific sequencing/quirks".

## Problem

Every family that speaks UDS-style diagnostics over CAN or K-Line re-implements
the same four things by hand: prepending the transport envelope, indexing past
it to reach the service byte, comparing that byte against `service + 0x40`, and
turning a negative response into a log line. The portable Colt CAN executor
(`src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp`) is the clearest
example — it carries a local `kServiceOffset = 4`, a local `positive()`, a local
`service_is()`, and a `nrc_context()` whose comment has to explain that
`QByteArray::mid` clamps. Roughly twenty exchange sites each repeat a
`size() > N` guard and two or three raw index comparisons.

No caller anywhere in the repository handles NRC `0x78`
(requestCorrectlyReceived-ResponsePending). Every one of them reports it as a
flat rejection, so an ECU that legitimately asks for more time aborts the
operation.

## Scope decisions

These were settled during design and bound everything below.

| Decision | Choice | Consequence |
| --- | --- | --- |
| First user | Retrofit the Mitsubishi Colt M32R CAN executor | It is already portable and carries a 1762-line byte-exact equivalence suite, so the refactor is verifiable without hardware |
| Framing scope | Application PDU only | The layer owns `[SID][subfunction][data]`; the 4-byte CAN-id envelope and any future K-Line header stay outside it, so only what is genuinely identical across transports is shared |
| I/O boundary | Codec plus a thin exchange helper | The helper owns one request/response round trip including `0x78`; sequencing and vendor quirks stay in executors |
| Equivalence | Behavior may change where UDS says so | Colt gains `0x78` handling and service-echo validation it never had; test expectations are updated rather than preserved |
| Dialect coverage | Shared core, UDS services only | The PDU model, negative-response model, and NRC decode are dialect-neutral; KWP-only services arrive with their first caller |

The layer is deliberately narrower than a session manager. `docs/tech-debt.md`
and the [protocol generalization notes](../../protocol-generalization-opportunities.md)
both draw the boundary at "share pure byte algorithms, framing, validation
primitives" and warn against forcing families into one configurable state
machine. Tester-present keepalive, session tracking, and security-access flow
templates are out of scope.

## Architecture

Three new Bazel packages, all portable — Qt-free, thread-free, filesystem-free —
plus one adapter in an existing package.

```
//src/algorithms/protocol/uds        pure codec, no I/O
//src/backend/protocol/uds           IUdsChannel port + UdsClient
//src/backend/protocol/uds/testing   ScriptedUdsChannel (ADR 0008)
//src/backend/flash                  CanFlashUdsChannel (new files in an existing package)
```

The adapter lives beside the transport it wraps, so the dependency runs
`flash -> protocol/uds`, the direction that already exists for
`src/backend/protocol/ikline_transport.h`.

Both new packages must be registered in the `genquery` in root `BUILD.bazel`
and in `PORTABLE_ROOTS` in `scripts/check-portable-closure.py`, or
`//:portable_closure` will not see them. `//src/backend/flash` is listed in
`QT_FREE_PACKAGES`, so `can_flash_uds_channel` is Qt-free by construction.

## The codec

`src/algorithms/protocol/uds/uds_pdu.h` exports the constants the layer acts on
structurally and the request builders:

```cpp
constexpr bytes::Byte kNegativeResponse = 0x7F;
constexpr bytes::Byte kPositiveResponseOffset = 0x40;
constexpr bytes::Byte kNrcResponsePending = 0x78;
constexpr bytes::Byte kNrcBusyRepeatRequest = 0x21;

constexpr bytes::Byte positiveResponse(bytes::Byte sid);
constexpr bytes::Byte requestFromPositive(bytes::Byte positive_sid);

bytes::Bytes buildRequest(bytes::Byte sid);
bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction);
bytes::Bytes buildRequest(bytes::Byte sid, bytes::ByteView data);
bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction, bytes::ByteView data);
```

Four overloads rather than one variadic: the subfunction-versus-first-data-byte
distinction is exactly what gets mis-read at call sites, and a named parameter
position keeps `buildRequest(kServiceSecurityAccess, 0x05)` unambiguous. All
four are built over `bytes::composeBe` per ADR 0013.

`src/algorithms/protocol/uds/uds_response.h` owns classification:

```cpp
enum class ResponseKind { Positive, Negative, Malformed };

struct Response
{
    ResponseKind kind{ResponseKind::Malformed};
    bytes::Byte service{};   // the *request* SID: 0x67 parses as 0x27
    bytes::Byte nrc{};       // meaningful only when kind == Negative
    bytes::ByteView data{};  // bytes after the response SID; a view into the input

    bool isPending() const;
    bool matches(bytes::Byte sid) const;
};

Response parseResponse(bytes::ByteView pdu);

bytes::ByteView payload(bytes::ByteView pdu);
std::optional<bytes::Byte> subfunction(bytes::ByteView pdu);
```

Classification is three rules applied in order:

1. `pdu[0] == 0x7F` is `Negative` and requires at least three bytes
   (`7F sid nrc`); shorter is `Malformed`.
2. `pdu[0] >= 0x40` is `Positive` with `service = pdu[0] - 0x40`.
3. Anything else, including an empty PDU, is `Malformed`.

Rule 1 cannot collide with a legitimate positive response, because that would
require request SID `0x3F`, which UDS reserves and no family uses. The header
states this so the next reader does not have to re-derive it.

`data` is a view into the caller's buffer rather than a copy. The only caller is
`UdsClient`, which parses a buffer it owns for the whole call; the field
documents the constraint. Executors do not call `parseResponse` directly — they
receive a validated positive-response PDU from the client and read it with
`payload()` and `subfunction()`.

NRC text is not re-tabulated. `describe()` delegates to `nrc_description()` in
`//src/algorithms/diagnostics`, keeping one authoritative table.

## The channel port and the client

```cpp
class IUdsChannel
{
  public:
    virtual ~IUdsChannel() = default;
    virtual Status send(bytes::ByteView pdu, const ICancellationToken&) = 0;
    virtual Result<std::optional<bytes::Bytes>> receive(int timeout_ms,
                                                        const ICancellationToken&) = 0;
};

struct ExchangePolicy
{
    int pre_read_delay_ms = 0;      // the legacy delay() between write and first read
    int read_timeout_ms = 500;
    int pending_timeout_ms = 3000;  // read timeout while the ECU holds us on 0x78
    int max_pending_repeats = 10;
};

class UdsClient
{
  public:
    UdsClient(IUdsChannel&, IClock&, IEventSink&);
    Result<bytes::Bytes> request(bytes::ByteView pdu, const ExchangePolicy&,
                                 const ICancellationToken&);
};
```

`IUdsChannel` implementations own the transport envelope; the client only ever
sees PDUs. The expected service is derived from `pdu[0]` and is never a separate
parameter — the client already holds the request, and a second parameter only
creates an opportunity to disagree with it.

`request()` returns the positive-response PDU, envelope-stripped, service byte
included.

### The pending rule is deliberately asymmetric

NRC `0x78` is absorbed: the client re-*reads* with `pending_timeout_ms`, up to
`max_pending_repeats` times, and **never re-sends**.

NRC `0x21` (busyRepeatRequest) is **not** absorbed, despite being the obvious
sibling, because absorbing it means re-transmitting the request — and on this
layer's first user that request may be RequestDownload, TransferData, or a
routine that erases flash. Re-sending a non-idempotent flash command on the
client's own initiative is the kind of hidden retry that turns a recoverable
error into a brick. `0x21` therefore surfaces as an ordinary negative response
and the executor decides. `kNrcBusyRepeatRequest` is still exported so a future
caller that knows its service is idempotent can implement the retry at its own
level, where the safety argument is visible in review.

### Error mapping

No new `ErrorKind` values. CLAUDE.md gates those on amending the step-5 design,
and every case lands naturally in the existing seven.

| Situation | Result |
| --- | --- |
| Channel `send`/`receive` fails | propagated verbatim |
| Nothing arrives within the timeout | `Timeout` |
| `Malformed` PDU | `BadResponse`, detail names the received bytes |
| Negative, not `0x78` | `BadResponse`, detail is `nrc_description()` of the frame |
| Positive but wrong service | `BadResponse`, "expected response to SID 0x27, got 0x50" |
| Still pending after `max_pending_repeats` | `Timeout`, "ECU still reporting responsePending after N repeats" |
| Cancelled | `Cancelled`, checked before send and honored by `IClock::sleep` |

The client emits exactly one `LogLevel::Debug` line per absorbed pending —
enough for a user watching a 30-second erase to see the ECU is alive, without
adding a log line to every ordinary exchange.

### CanFlashUdsChannel

Constructed from `ICanFlashTransport&` plus the plan's `request_id` and
`response_id`.

- `send` produces `bytes::composeBe(request_id_, pdu)` — byte-identical to the
  Colt executor's current `build_request()`.
- `receive` reads, passes an empty optional through unchanged on timeout, and
  otherwise requires at least four bytes whose big-endian value equals
  `response_id_`, returning `subspan(4)`.

Validating the reply id is new; the executor never checked it. The plan has
declared `response_id = 0x7e8` since it was written and the J2534 ISO-15765
filter should make a mismatch impossible, so the check costs nothing and turns a
silent mis-parse into a typed error.

## The Colt retrofit

`execute()` constructs the `CanFlashUdsChannel` over the transport it already
opens, using the plan's `request_id` and `response_id`, and constructs the
`UdsClient` over that channel plus the injected `IClock` and `IEventSink`. Both
are stack-local to `execute()` and outlive every exchange; nothing new is
allocated and no ownership crosses a port.

`Ctx` keeps `transport` for lifecycle (`configure`, `open`, `close`,
`request_unblock`) and `clock` for the non-exchange waits that remain
(post-erase settling), and gains a `UdsClient&`. Six local helpers in
`mitsu_colt_m32r_can_executor.cpp` are deleted: `kServiceOffset`, `positive()`,
`build_request()`, `nrc_context()`, `service_is()`, and `exchange()`.

Each call site collapses from a build/write/sleep/read/length-check/
service-check/subfunction-check stack to:

```cpp
Result<bytes::Bytes> received = ctx.uds.request(
    buildDiagnosticSession(kSessionBasic),
    {.pre_read_delay_ms = 50, .read_timeout_ms = kReadTimeoutMs}, ctx.cancellation);
if (!received.has_value())
{
    error(ctx, std::format("Wrong response from ECU: {}", received.error().detail));
    return std::unexpected(received.error());
}
if (uds::subfunction(*received) != kSessionBasic)
{
    // ...
}
```

Offsets that used to be written against an envelope the reader had to remember
become readable against the UDS frame layout: `received->subspan(7, 4)` for the
vendor seed becomes `payload(*received).subspan(2, 4)`.

### The wire stays byte-identical

`CanFlashUdsChannel::send` emits exactly what `build_request()` emitted, so every
`expectWrite()` in `mitsu_colt_m32r_can_executor_test.cpp` is untouched, and
every `queueRead(response({...}))` is untouched because the adapter strips the
same four bytes the executor used to index past. That suite is the regression
gate for this refactor and it survives intact.

The log diff is smaller than it first appears. At negative-response sites the
executor still formats `"Wrong response from ECU: {}"` and the client's `detail`
is `nrc_description()` of the same frame, so those lines come out
character-identical. Only sites that previously hit a malformed or wrong-service
frame change, from `"Wrong response from ECU: Not a valid answer"` to
`"...: expected response to SID 0x10, got 0x50"`.

### Behavior changes that ship

1. `0x78` is absorbed and re-read instead of aborting the operation.
2. Malformed and wrong-service frames get typed detail instead of
   "Not a valid answer".
3. The reply CAN id is validated rather than skipped.
4. The service echo is checked at every exchange; two sites previously
   length-checked only and become stricter.

### Colt builder bodies

`MitsuColtCan` and `MitsuColtCanVendorExt` builder **bodies** are re-expressed
over `uds::buildRequest`. Headers, signatures, and output bytes are unchanged,
and the existing byte-level tests in `//src/algorithms/protocol/colt` are the
proof. This is what demonstrates the generic core is reusable rather than merely
adjacent, and it leaves one composition path instead of two.

## Result-checking convention

`Result` and `Status` are checked with `.has_value()`, never the implicit
`operator bool`. This is already the house majority — roughly 1203 `.has_value()`
uses across `src/backend` and `src/algorithms` against about 164 operator-bool
sites — so the Colt executor is drifting from the convention rather than
defining a new one.

- All new code in the three UDS packages uses `.has_value()`, including the
  `if (Status s = ...; !s)` idiom, which becomes a plain declaration followed by
  `if (!s.has_value())`; the init-statement form only reads well with the
  implicit conversion.
- Every operator-bool check in the files this change touches is converted in the
  same change, roughly ten sites in `mitsu_colt_m32r_can_executor.cpp`.
- **ADR 0014, "check Result with has_value"** is added alongside the existing
  0009-0013 style ADRs.
- A repo-wide sweep of the remaining sites is **out of scope** and is filed as a
  follow-up issue, so this change does not grow a 150-file diff that buries the
  protocol work.

## Testing

Four `fastecu_portable_gtest` layers, plus the existing executor suite as the
regression gate.

1. **`//src/algorithms/protocol/uds`** — table-driven codec tests over
   hand-written frames: every `buildRequest` overload, all three classification
   rules, and the malformed cases `{}`, `{0x7F}`, `{0x7F, 0x27}`, `{0x10}`.
2. **`//src/backend/protocol/uds`** — client tests against
   `ScriptedUdsChannel`: pending absorbed once, absorbed repeatedly, exhausted
   at `max_pending_repeats`, `0x21` **not** retried (asserting the channel sees
   exactly one send — this is the safety property, so it gets an explicit
   negative test), wrong-service, malformed, timeout, channel error propagated
   verbatim, cancellation before send, and `FakeClock` observing
   `pre_read_delay_ms`.
3. **`//src/backend/protocol/uds/testing`** — the mock's own test, per ADR 0008.
4. **`//src/backend/flash`** — `CanFlashUdsChannel` against
   `ScriptedCanFlashTransport`: envelope added on send, stripped on receive,
   empty optional passed through on timeout, short frame and reply-id mismatch
   rejected.

New executor tests cover pending-absorbed, pending-exhausted, wrong-service, and
reply-id-mismatch, none of which the suite can express today.

## Hardware-facing risk

`FlashEcuMitsuM32rCan` is `hardware_status: experimental`, gated by the
[Colt bench checklist](../../colt_czt_47110032_can_bench_checklist.md) before
real-vehicle use. Changing abort semantics on an already un-qualified path is
the right place for it, but behavior change 1 alters how a live flash behaves
when the ECU reports busy. The change therefore includes:

- an updated row in the [flash qualification matrix](../../flash-qualification-matrix.md)
  noting the pending-retry path,
- a checklist line recording that pending-retry is unexercised on hardware.

## Delivery order

Each step green before the next:

1. Codec package.
2. Client, channel port, and `ScriptedUdsChannel`.
3. `CanFlashUdsChannel`.
4. Colt builder bodies re-expressed (colt tests prove no byte changed).
5. Executor retrofit.
6. ADR 0014, qualification-matrix row, checklist line, follow-up issue for the
   operator-bool sweep.

## Out of scope

- Session management: tester-present keepalive, session tracking, security
  access flow templates.
- KWP2000-only services and layouts (StartCommunication `0x81`, the KWP `0x23`
  addressing layout). They arrive with their first caller, against the same
  dialect-neutral core.
- Migrating the roughly ten legacy Subaru Denso/Hitachi CAN operations under
  `src/platform/desktop/common/flash/legacy/ecu/`. The step-5 tail rule ports a
  family to a tested portable executor first and extracts second; those are
  still untested Qt sources.
- Automatic `0x21` retry.
- The repo-wide operator-bool sweep.
