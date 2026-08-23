# J2534 Serial Throughput Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make bulk CAN reads against a Mitsubishi Colt CZT ECU run near CAN wire speed by replacing the Unix J2534 layer's per-byte serial I/O with buffered I/O, and make ISO-TP configuration failures observable instead of silent.

**Architecture:** A `SerialByteBuffer` staging buffer sits between the Openport's byte-oriented Tactrix protocol parser and `QSerialPort`. Call sites keep reading one byte at a time — the parser scans for `\r\n` terminators and cannot be batched — but a one-byte read becomes a buffer index instead of a syscall plus a 1 ms event-loop wait. The transmit side collapses a per-byte write loop into one write plus an explicit flush. Separately, `PassThruIoctl(SET_CONFIG)` starts parsing the adapter's reply so a rejected parameter surfaces as an error.

**Tech Stack:** C++23, Qt 6.8.3 (`QSerialPort`, `QByteArray`), Bazel 9.1.1, GoogleTest.

**Spec:** [J2534 serial throughput on Unix — design](../specs/2026-08-23-j2534-serial-throughput-design.md)

## Global Constraints

- **Platform scope:** macOS and Linux only. Every source change is confined to `src/platform/desktop/unix/j2534/` and `apps/bench/`. Windows reaches the adapter through a vendor DLL (`src/platform/desktop/windows/j2534/`) and must not be touched.
- **Branch:** all work lands on `perf/j2534-serial-throughput`, which already exists and already carries the design doc commit. Per `CLAUDE.md`, FastECU work goes through a pull request — never commit to `master`.
- **Read-only until Task 9.** Tasks 1–8 issue no destructive PDU. No task in this plan relaxes an address-window guard, changes a flash write path, or marks anything bench-qualified.
- **Build/test commands:**
  - `bazel build --config=release //apps/bench:fastecu-bench`
  - `bazel test --config=release //...`
  - `prek run --all-files`
  - The binary must be launched via `bazel run --config=release //apps/bench:fastecu-bench -- <args>`. Invoking `./bazel-bin/apps/bench/fastecu-bench` directly fails with a dyld `QtOpenGLWidgets` rpath error.
- **Error conventions:** backend code returns `fastecu::Result<T>` checked with `.has_value()`, never the implicit `operator bool`. `J2534` methods are legacy C-style and return `STATUS_NOERROR` (0) / `ERR_FAILED` (7) from `J2534_tactrix_unix.h`.
- **Style:** `#pragma once` in every header (enforced by prek). Prefer `std::string_view` by value, `std::format` for message construction. Markdown cross-references are links with human-readable text, never backticked paths — lychee runs in prek and cannot see a path written as inline code.
- **Hardware facts for this bench:** OpenPort 2.0 on `/dev/cu.usbmodemTApU_RJO1`. ECU is a Colt CZT (Z37A, ROM 47110032) carrying the third-party vendor diagnostic extension. CAN 500 kbit/s, request id `0x7E0`, response id `0x7E8`, 11-bit.

---

### Task 1: `--vendor-ext` command-line flag

The bench ECU does not answer a bare bootload-session request; it requires the vendor challenge first. This task adds only the flag. Task 2 makes it do something.

**Files:**
- Modify: `apps/bench/bench_args.h:22-30` (`GlobalOptions`)
- Modify: `apps/bench/bench_args.cpp:144-230` (`parse_command_line`)
- Test: `apps/bench/bench_args_test.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `GlobalOptions::vendor_ext` (`bool`, default `false`), set by the global flag `--vendor-ext`.

- [ ] **Step 1: Write the failing tests**

Append to `apps/bench/bench_args_test.cpp`, inside the existing `namespace fastecu::bench { namespace { ... } }` block. Check how neighbouring tests build their `std::vector<std::string_view>` argument list and follow that shape exactly; the helper below matches the file's existing style.

```cpp
TEST(BenchArgs, VendorExtDefaultsToOff)
{
    const std::vector<std::string_view> args{"read", "0x200", "1"};
    const Result<ParsedCommandLine> parsed = parse_command_line(args);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->options.vendor_ext);
}

TEST(BenchArgs, VendorExtFlagIsRecognisedAnywhereOnTheCommandLine)
{
    const std::vector<std::string_view> args{"read", "0x200", "1", "--vendor-ext"};
    const Result<ParsedCommandLine> parsed = parse_command_line(args);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->options.vendor_ext);
    // The flag is global, not a step argument: it must not reach the step.
    ASSERT_EQ(parsed->steps.size(), 1u);
    EXPECT_EQ(parsed->steps.front().args.size(), 2u);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```sh
bazel test --config=release //apps/bench:bench_args_test
```

Expected: FAIL — compile error, `GlobalOptions` has no member `vendor_ext`.

- [ ] **Step 3: Add the field**

In `apps/bench/bench_args.h`, add to `GlobalOptions` after `bool no_connect = false;`:

```cpp
    // Runs the third-party vendor diagnostic challenge before the bootload
    // session. Required by ROMs carrying that extension, which do not answer
    // a bare 0x10 0x85; stock ROMs must not be sent it.
    bool vendor_ext = false;
```

- [ ] **Step 4: Parse the flag**

In `apps/bench/bench_args.cpp`, inside `parse_command_line`'s argument loop, next to the existing `--no-connect` branch:

```cpp
        if (arg == "--vendor-ext")
        {
            parsed.options.vendor_ext = true;
            continue;
        }
```

- [ ] **Step 5: Run the tests to verify they pass**

```sh
bazel test --config=release //apps/bench:bench_args_test
```

Expected: PASS.

- [ ] **Step 6: Document the flag in the known-limitations list**

`docs/bench-cli-checklist.md` section 4 lists which global options belong on the outer invocation when `--script -` is used. Replace:

```markdown
- [ ] With `--script -`, global options (`--port`, `--json`, `--verbose`,
      `--timeout`, `--keep-going`, `--no-connect`, `--script`) belong on the
      outer invocation.
```

with:

```markdown
- [ ] With `--script -`, global options (`--port`, `--json`, `--verbose`,
      `--timeout`, `--keep-going`, `--no-connect`, `--vendor-ext`,
      `--script`) belong on the outer invocation.
```

Leave the rest of that bullet's text untouched.

- [ ] **Step 7: Commit**

```sh
git add apps/bench/bench_args.h apps/bench/bench_args.cpp \
        apps/bench/bench_args_test.cpp docs/bench-cli-checklist.md
git commit -m "feat(bench): add --vendor-ext global flag"
```

---

### Task 2: Vendor-extension challenge in `BenchSession::connect()`

`BenchSession::connect()` currently sends `buildDiagnosticSession(kSessionBootload)` as its first PDU. The desktop executor's `connect_bootloader` (`src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp:100-152`) instead opens a **basic** session, completes the vendor challenge, and only then enters the bootload session. Mirror that sequence.

**Files:**
- Modify: `apps/bench/bench_session.h:22-30` (constructor signature, new member)
- Modify: `apps/bench/bench_session.cpp:77-142` (constructor, `connect`)
- Modify: `apps/bench/BUILD.bazel` (`bench_session_runtime` deps)
- Test: `apps/bench/bench_session_test.cpp`

**Interfaces:**
- Consumes: `GlobalOptions::vendor_ext` from Task 1 (wired in Task 3).
- Produces: `BenchSession(std::unique_ptr<flash::ICanFlashTransport>, std::uint32_t request_id, std::uint32_t response_id, IClock&, IEventSink&, const ICancellationToken&, bool vendor_challenge = false)`. The trailing parameter defaults to `false` so existing call sites and tests compile unchanged.

Protocol facts, all from `src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h`:

- Seed request PDU: `buildChallengeSeedRequest()` → `[0x23][0x27][0x41]`.
- Positive reply SID is `0x63`; `uds::payload()` strips it, leaving `[0x27][0x41][seed0..seed3]` — exactly 6 bytes.
- Key PDU: `buildChallengeKey(key)` → `[0x23][0x27][0x42][key0..key3]`.
- Key reply payload: `[0x27][0x34]`. `kVendorChallengeAccepted` is `0x34`. Echoing the selector `0x27` alone is **not** acceptance — this must be its own content check, matching the executor.

- [ ] **Step 1: Write the failing tests**

Add to `apps/bench/bench_session_test.cpp`. Extend the existing `Harness` struct with a constructor parameter and the three vendor expectations, then add the tests. Note `Harness`'s existing member initialisation order — `session` is constructed in the body, so the flag can be a constructor argument:

```cpp
// --- extend the existing Harness ---
    explicit Harness(bool vendor_challenge = false)
    {
        auto owned = std::make_unique<flash::ScriptedCanFlashTransport>();
        transport = owned.get();
        session = std::make_unique<BenchSession>(std::move(owned), kRequestId, kResponseId, clock, events,
                                                 cancellation, vendor_challenge);
    }

    void expectBasicSession(bytes::Byte echoed_session = MitsuColtCan::kSessionBasic)
    {
        transport->expectWrite(request(MitsuColtCan::buildDiagnosticSession(MitsuColtCan::kSessionBasic)));
        transport->queueRead(response(bytes::Bytes{0x50, echoed_session}));
    }

    void expectVendorSeed()
    {
        transport->expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
        transport->queueRead(response(bytes::composeBe(bytes::Byte{0x63},
                                                       MitsuColtCanVendorExt::kVendorChallengeSelector,
                                                       MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction,
                                                       kVendorSeed)));
    }

    void expectVendorKey(bytes::Byte accepted = MitsuColtCanVendorExt::kVendorChallengeAccepted)
    {
        const std::uint32_t key =
            MitsuColtCanVendorExt::challengeInverseTransform(MitsuColtCanVendorExt::bytesToSeed(kVendorSeed));
        transport->expectWrite(request(MitsuColtCanVendorExt::buildChallengeKey(key)));
        transport->queueRead(response(
            bytes::Bytes{0x63, MitsuColtCanVendorExt::kVendorChallengeSelector, accepted}));
    }
```

Add this constant next to the existing `kSeed`:

```cpp
const bytes::Bytes kVendorSeed{0xDE, 0xAD, 0xBE, 0xEF};
```

Add this include next to the existing protocol include:

```cpp
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
```

Then the tests:

```cpp
TEST(BenchSession, VendorChallengeIsSkippedWhenNotRequested)
{
    Harness harness;
    harness.expectSession();
    harness.expectSeed();
    harness.expectKey();

    const Status result = harness.session->connect();

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(harness.transport->scriptConsumed());
    EXPECT_EQ(harness.session->last_traffic().exchange_count, 3u);
}

TEST(BenchSession, VendorChallengePrecedesTheBootloadSessionInOrder)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.expectVendorSeed();
    harness.expectVendorKey();
    harness.expectSession();
    harness.expectSeed();
    harness.expectKey();

    const Status result = harness.session->connect();

    ASSERT_TRUE(result.has_value());
    // ScriptedCanFlashTransport rejects any write that does not match the next
    // expectation in order, so a green script IS the ordering assertion.
    EXPECT_TRUE(harness.transport->scriptConsumed());
    EXPECT_EQ(harness.session->last_traffic().exchange_count, 6u);
}

TEST(BenchSession, VendorChallengeRejectsAKeyReplyThatOnlyEchoesTheSelector)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.expectVendorSeed();
    // 0x00 in place of kVendorChallengeAccepted: the selector still echoes,
    // but the ECU has not granted the transition.
    harness.expectVendorKey(0x00);

    const Status result = harness.session->connect();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
}

TEST(BenchSession, VendorChallengeRejectsAShortSeedReply)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.transport->expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    // Selector bytes present but only two seed bytes behind them.
    harness.transport->queueRead(response(bytes::Bytes{
        0x63, MitsuColtCanVendorExt::kVendorChallengeSelector,
        MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction, 0xDE, 0xAD}));

    const Status result = harness.session->connect();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
}
```

If `Error`'s member is not named `kind`, read `src/backend/ports/error.h` and use the real name; keep the assertion on `ErrorKind::BadResponse` either way.

- [ ] **Step 2: Run the tests to verify they fail**

```sh
bazel test --config=release //apps/bench:bench_session_test
```

Expected: FAIL — compile error, `BenchSession` constructor takes 6 arguments, not 7.

- [ ] **Step 3: Add the constructor parameter and member**

In `apps/bench/bench_session.h`, change the constructor declaration to:

```cpp
    BenchSession(std::unique_ptr<flash::ICanFlashTransport> transport, std::uint32_t request_id,
                 std::uint32_t response_id, IClock& clock, IEventSink& events, const ICancellationToken& cancellation,
                 bool vendor_challenge = false);
```

and add, after `TrafficEvidence last_traffic_;`:

```cpp
    bool vendor_challenge_;
```

- [ ] **Step 4: Implement the challenge**

In `apps/bench/bench_session.cpp`, add the include next to the existing protocol include:

```cpp
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
```

Update the constructor's initialiser list — append `, vendor_challenge_(vendor_challenge)` after `cancellation_(cancellation)` — and change its signature to match the header.

Then, in `connect()`, insert this block immediately after the `request` lambda is defined and **before** the existing `buildDiagnosticSession(MitsuColtCan::kSessionBootload)` call:

```cpp
    if (vendor_challenge_)
    {
        const Result<bytes::Bytes> basic_reply =
            request(MitsuColtCan::buildDiagnosticSession(MitsuColtCan::kSessionBasic));
        if (!basic_reply.has_value())
        {
            return std::unexpected(basic_reply.error());
        }
        if (const Status valid =
                validateEcho(*basic_reply, MitsuColtCan::kSessionBasic, 1, "basic diagnostic session");
            !valid.has_value())
        {
            return valid;
        }

        const Result<bytes::Bytes> vendor_seed_reply =
            request(MitsuColtCanVendorExt::buildChallengeSeedRequest());
        if (!vendor_seed_reply.has_value())
        {
            return std::unexpected(vendor_seed_reply.error());
        }
        // [selector][subfunction][4-byte seed].
        const bytes::ByteView vendor_seed_payload = uds::payload(*vendor_seed_reply);
        if (vendor_seed_payload.size() < 6)
        {
            return fail(ErrorKind::BadResponse, "vendor challenge seed reply too short");
        }
        if (vendor_seed_payload[0] != MitsuColtCanVendorExt::kVendorChallengeSelector ||
            vendor_seed_payload[1] != MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction)
        {
            return fail(ErrorKind::BadResponse,
                        std::format("vendor challenge seed reply carried 0x{:02x} 0x{:02x}",
                                    vendor_seed_payload[0], vendor_seed_payload[1]));
        }

        const std::uint32_t vendor_key = MitsuColtCanVendorExt::challengeInverseTransform(
            MitsuColtCanVendorExt::bytesToSeed(vendor_seed_payload.subspan(2, 4)));

        const Result<bytes::Bytes> vendor_key_reply =
            request(MitsuColtCanVendorExt::buildChallengeKey(vendor_key));
        if (!vendor_key_reply.has_value())
        {
            return std::unexpected(vendor_key_reply.error());
        }
        const bytes::ByteView vendor_key_payload = uds::payload(*vendor_key_reply);
        if (vendor_key_payload.size() < 2)
        {
            return fail(ErrorKind::BadResponse, "vendor challenge key reply too short");
        }
        // Echoing the selector is not acceptance: only kVendorChallengeAccepted
        // grants the transition, so this stays a content check of its own.
        if (vendor_key_payload[1] != MitsuColtCanVendorExt::kVendorChallengeAccepted)
        {
            return fail(ErrorKind::BadResponse,
                        std::format("vendor challenge key rejected: reply 0x{:02x}", vendor_key_payload[1]));
        }
    }
```

- [ ] **Step 5: Add the Bazel dependency**

In `apps/bench/BUILD.bazel`, the `bench_session_runtime` target already depends on `//src/algorithms/protocol/colt:mitsu_colt_can_protocol`. Add alongside it:

```python
        "//src/algorithms/protocol/colt:mitsu_colt_can_vendor_ext_protocol",
```

Add the same dep to the `bench_session_test` target.

- [ ] **Step 6: Run the tests to verify they pass**

```sh
bazel test --config=release //apps/bench:bench_session_test
```

Expected: PASS, all tests including the pre-existing ones.

- [ ] **Step 7: Commit**

```sh
git add apps/bench/bench_session.h apps/bench/bench_session.cpp \
        apps/bench/bench_session_test.cpp apps/bench/BUILD.bazel
git commit -m "feat(bench): run the vendor challenge before the bootload session"
```

---

### Task 3: Wire the flag through `main.cpp` and confirm against hardware

This is the first task that touches the ECU. It is read-only.

**Files:**
- Modify: `apps/bench/main.cpp:61-95` (`DesktopBenchEnvironment::session`)

**Interfaces:**
- Consumes: `GlobalOptions::vendor_ext` (Task 1), the 7-argument `BenchSession` constructor (Task 2).
- Produces: a working session against the bench ECU — the precondition for every measurement in this plan.

- [ ] **Step 1: Pass the flag at construction**

In `apps/bench/main.cpp`, in `DesktopBenchEnvironment::session`, the `session_.emplace(...)` call currently ends with `cancellation_`. Add the flag as the final argument:

```cpp
        session_.emplace(std::move(*transport), kCanConfig.request_id, kCanConfig.response_id, clock_, events_,
                         cancellation_, options.vendor_ext);
```

- [ ] **Step 2: Build**

```sh
bazel build --config=release //apps/bench:fastecu-bench
```

Expected: success.

- [ ] **Step 3: Confirm the adapter is visible**

```sh
bazel run --config=release //apps/bench:fastecu-bench -- ports
```

Expected: a line reading `cu.usbmodemTApU_RJO1 - OpenPort 2.0`.

- [ ] **Step 4: Confirm the regression — without the flag, nothing answers**

```sh
bazel run --config=release //apps/bench:fastecu-bench -- --verbose read 0x8056a8 16
```

Expected: `FAIL (Timeout) no response within the read timeout`, with `TX first 10 85` and an empty `RX first`. This is the documented pre-existing behaviour; record the output.

- [ ] **Step 5: Confirm the fix — with the flag, the session opens and data comes back**

```sh
bazel run --config=release //apps/bench:fastecu-bench -- --vendor-ext --verbose read 0x8056a8 16
```

Expected: `ok`, with 16 bytes under `DATA`. If it still fails, **stop and report** — do not proceed to the performance work, and do not start adjusting timeouts or CAN ids speculatively. A failure here is a protocol problem: invoke the `superpowers:systematic-debugging` skill rather than guessing.

- [ ] **Step 6: Run the full suite and linters**

```sh
bazel test --config=release //...
prek run --all-files
```

Expected: green.

- [ ] **Step 7: Commit**

```sh
git add apps/bench/main.cpp
git commit -m "feat(bench): wire --vendor-ext into the desktop session"
```

---

### Task 4: `--stats` derived throughput metrics

Baselines need to be comparable without arithmetic at the terminal.

**Files:**
- Modify: `apps/bench/bench_args.h` (`GlobalOptions`), `apps/bench/bench_args.cpp` (`parse_command_line`)
- Modify: `apps/bench/bench_format.h`, `apps/bench/bench_format.cpp`
- Modify: `apps/bench/bench_driver.cpp:54-57` (`emit`)
- Test: `apps/bench/bench_format_test.cpp`, `apps/bench/bench_args_test.cpp`

**Interfaces:**
- Consumes: `CommandOutcome::data`, `::elapsed_ms`, `::exchange_count` (all already present in `apps/bench/bench_types.h`).
- Produces:
  - `GlobalOptions::stats` (`bool`, default `false`), set by `--stats`.
  - `std::string format_text(const CommandOutcome& outcome, bool stats = false)`
  - `std::string format_json(const CommandOutcome& outcome, bool stats = false)`

Rules, from the spec: `bytes_per_s` is `data.size()` over `elapsed_ms`; `ms_per_exchange` is `elapsed_ms` over `exchange_count`. Each is **omitted** rather than reported as zero when its inputs are absent — no data, or no exchanges. `elapsed_ms == 0` also omits `bytes_per_s`, since dividing by it is undefined rather than infinitely fast.

- [ ] **Step 1: Write the failing tests**

Add to `apps/bench/bench_format_test.cpp`:

```cpp
TEST(BenchFormat, StatsAreOmittedUnlessRequested)
{
    const CommandOutcome outcome{
        .step = "read 0x200 4", .exchange_count = 2, .data = bytes::Bytes{1, 2, 3, 4}, .elapsed_ms = 100, .ok = true};

    EXPECT_EQ(format_text(outcome).find("bytes/s"), std::string::npos);
    EXPECT_EQ(format_json(outcome).find("bytes_per_s"), std::string::npos);
}

TEST(BenchFormatStats, JsonCarriesBothDerivedFigures)
{
    const CommandOutcome outcome{
        .step = "read 0x200 4", .exchange_count = 2, .data = bytes::Bytes{1, 2, 3, 4}, .elapsed_ms = 100, .ok = true};

    const std::string json = format_json(outcome, true);

    // 4 bytes / 0.100 s = 40 bytes/s; 100 ms / 2 exchanges = 50 ms.
    EXPECT_NE(json.find(R"("bytes_per_s":40.0)"), std::string::npos);
    EXPECT_NE(json.find(R"("ms_per_exchange":50.0)"), std::string::npos);
}

TEST(BenchFormatStats, BytesPerSecondIsOmittedWithoutData)
{
    const CommandOutcome outcome{.step = "erase", .exchange_count = 1, .elapsed_ms = 100, .ok = true};

    const std::string json = format_json(outcome, true);

    EXPECT_EQ(json.find("bytes_per_s"), std::string::npos);
    EXPECT_NE(json.find(R"("ms_per_exchange":100.0)"), std::string::npos);
}

TEST(BenchFormatStats, BytesPerSecondIsOmittedWhenNoTimeElapsed)
{
    const CommandOutcome outcome{
        .step = "read 0x200 4", .exchange_count = 1, .data = bytes::Bytes{1, 2, 3, 4}, .elapsed_ms = 0, .ok = true};

    const std::string json = format_json(outcome, true);

    EXPECT_EQ(json.find("bytes_per_s"), std::string::npos);
}

TEST(BenchFormatStats, MsPerExchangeIsOmittedWithoutExchanges)
{
    const CommandOutcome outcome{.step = "ports", .exchange_count = 0, .elapsed_ms = 5, .ok = true};

    const std::string json = format_json(outcome, true);

    EXPECT_EQ(json.find("ms_per_exchange"), std::string::npos);
}

TEST(BenchFormatStats, TextModePrintsAnIndentedStatsLine)
{
    const CommandOutcome outcome{
        .step = "read 0x200 4", .exchange_count = 2, .data = bytes::Bytes{1, 2, 3, 4}, .elapsed_ms = 100, .ok = true};

    EXPECT_NE(format_text(outcome, true).find("  40.0 bytes/s, 50.0 ms/exchange\n"), std::string::npos);
}
```

Add to `apps/bench/bench_args_test.cpp`:

```cpp
TEST(BenchArgs, StatsFlagIsRecognised)
{
    const std::vector<std::string_view> args{"--stats", "read", "0x200", "1"};
    const Result<ParsedCommandLine> parsed = parse_command_line(args);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->options.stats);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```sh
bazel test --config=release //apps/bench:bench_format_test //apps/bench:bench_args_test
```

Expected: FAIL — compile errors, `format_text` takes one argument and `GlobalOptions` has no `stats`.

- [ ] **Step 3: Add the flag**

In `apps/bench/bench_args.h`, add to `GlobalOptions`:

```cpp
    // Prints derived throughput figures alongside each outcome.
    bool stats = false;
```

In `apps/bench/bench_args.cpp`, next to the `--vendor-ext` branch from Task 1:

```cpp
        if (arg == "--stats")
        {
            parsed.options.stats = true;
            continue;
        }
```

- [ ] **Step 4: Implement the formatting**

In `apps/bench/bench_format.h`, change both declarations to take the flag and document it:

```cpp
// Human-readable rendering: one step per block, first/last tx/rx as spaced hex.
// With `stats`, appends an indented derived-throughput line.
std::string format_text(const CommandOutcome& outcome, bool stats = false);

// One flat JSON object per step, including first/last traffic, count and
// elapsed time. Hex has no separators so an agent can slice it directly.
// Emitted on stdout; all logging goes to stderr. With `stats`, adds
// "bytes_per_s" and "ms_per_exchange" -- each omitted when its inputs are
// absent, so a missing key means "not measurable", never "zero".
std::string format_json(const CommandOutcome& outcome, bool stats = false);
```

In `apps/bench/bench_format.cpp`, add to the anonymous namespace:

```cpp
std::optional<double> bytesPerSecond(const CommandOutcome& outcome)
{
    if (outcome.data.empty() || outcome.elapsed_ms == 0)
    {
        return std::nullopt;
    }
    return static_cast<double>(outcome.data.size()) * 1000.0 / static_cast<double>(outcome.elapsed_ms);
}

std::optional<double> msPerExchange(const CommandOutcome& outcome)
{
    if (outcome.exchange_count == 0)
    {
        return std::nullopt;
    }
    return static_cast<double>(outcome.elapsed_ms) / static_cast<double>(outcome.exchange_count);
}
```

Add `#include <optional>` to the include block.

In `format_text`, immediately before the final `out += outcome.ok ? ...` line:

```cpp
    if (stats)
    {
        const std::optional<double> rate = bytesPerSecond(outcome);
        const std::optional<double> per_exchange = msPerExchange(outcome);
        if (rate.has_value() && per_exchange.has_value())
        {
            out += std::format("  {:.1f} bytes/s, {:.1f} ms/exchange\n", *rate, *per_exchange);
        }
        else if (rate.has_value())
        {
            out += std::format("  {:.1f} bytes/s\n", *rate);
        }
        else if (per_exchange.has_value())
        {
            out += std::format("  {:.1f} ms/exchange\n", *per_exchange);
        }
    }
```

In `format_json`, immediately before `out += "}";`:

```cpp
    if (stats)
    {
        if (const std::optional<double> rate = bytesPerSecond(outcome); rate.has_value())
        {
            out += std::format(R"(,"bytes_per_s":{:.1f})", *rate);
        }
        if (const std::optional<double> per_exchange = msPerExchange(outcome); per_exchange.has_value())
        {
            out += std::format(R"(,"ms_per_exchange":{:.1f})", *per_exchange);
        }
    }
```

- [ ] **Step 5: Pass the flag through the driver**

In `apps/bench/bench_driver.cpp`, change `emit`:

```cpp
void emit(const GlobalOptions& options, const CommandOutcome& outcome, std::ostream& output)
{
    const std::string rendered =
        options.json ? format_json(outcome, options.stats) : format_text(outcome, options.stats);
```

Leave the rest of the function untouched. Every existing `emit(options, ...)` call site already passes `options`, so no other change is needed.

- [ ] **Step 6: Run the tests to verify they pass**

```sh
bazel test --config=release //apps/bench:bench_format_test //apps/bench:bench_args_test //apps/bench:bench_driver_test
```

Expected: PASS.

- [ ] **Step 7: Document the flag**

In `docs/bench-cli-checklist.md` section 4, add `--stats` to the same parenthesised list Task 1 amended, so it reads:

```markdown
- [ ] With `--script -`, global options (`--port`, `--json`, `--verbose`,
      `--timeout`, `--keep-going`, `--no-connect`, `--vendor-ext`,
      `--stats`, `--script`) belong on the outer invocation.
```

- [ ] **Step 8: Commit**

```sh
git add apps/bench/bench_args.h apps/bench/bench_args.cpp apps/bench/bench_args_test.cpp \
        apps/bench/bench_format.h apps/bench/bench_format.cpp apps/bench/bench_format_test.cpp \
        apps/bench/bench_driver.cpp docs/bench-cli-checklist.md
git commit -m "feat(bench): add --stats derived throughput figures"
```

---

### Task 5: Record the baseline

No source changes. This produces the number every later task is judged against.

**Files:**
- Create: `docs/j2534-throughput-bench-notes.md`

**Interfaces:**
- Consumes: `--vendor-ext` (Task 3), `--stats` (Task 4).
- Produces: a recorded pre-change throughput figure.

- [ ] **Step 1: Build the current binary**

```sh
bazel build --config=release //apps/bench:fastecu-bench
```

- [ ] **Step 2: Measure a bulk read**

8 KiB spans 43 chunks at `MitsuColtCan::kFlashReadBlockSize` (192 bytes), which is enough for per-chunk overhead to dominate startup cost.

```sh
bazel run --config=release //apps/bench:fastecu-bench -- \
  --vendor-ext --stats --json read 0x8056a8 8192
```

This is expected to be slow — on the order of several seconds. Record the full JSON line verbatim.

- [ ] **Step 3: Repeat twice more**

Run the identical command two more times. Record all three lines. If the three `bytes_per_s` figures differ by more than about 20%, note that spread — it matters when judging later results.

- [ ] **Step 4: Write the notes file**

Create `docs/j2534-throughput-bench-notes.md`:

```markdown
# J2534 serial throughput — bench measurements

Measurements backing the [J2534 serial throughput design](superpowers/specs/2026-08-23-j2534-serial-throughput-design.md).

Every figure here is a real measurement against real hardware. Do not add a
projected or calculated number to this file without labelling it as such.

## Setup

- Adapter: Tactrix OpenPort 2.0, `/dev/cu.usbmodemTApU_RJO1`
- ECU: Colt CZT (Z37A, ROM 47110032), spare/bench unit, vendor diagnostic
  extension present
- Bus: CAN 500 kbit/s, 11-bit, request `0x7E0` / response `0x7E8`
- Supply voltage: FILL IN — read it off the bench supply, do not guess
- Host: macOS, Apple Silicon
- Command: `fastecu-bench --vendor-ext --stats --json read 0x8056a8 8192`
  (8192 bytes = 43 chunks at `kFlashReadBlockSize` = 192)

## Reference: what the bus itself can do

One 192-byte chunk is one ISO-TP FirstFrame plus 27 ConsecutiveFrames. At
500 kbit/s and roughly 135 bits per frame that is about 7.6 ms of wire time,
a ceiling near 25 KB/s. This is arithmetic, not a measurement.

## Baseline — before any change

Commit: FILL IN (`git rev-parse --short HEAD`)

| Run | ms | exchanges | bytes/s | ms/exchange |
| --- | --- | --- | --- | --- |
| 1 | | | | |
| 2 | | | | |
| 3 | | | | |

Raw JSON:

```
FILL IN — the three lines verbatim
```
```

Replace every `FILL IN` with the real value. A `FILL IN` left in this file is a failed task.

- [ ] **Step 5: Verify the links resolve**

```sh
prek run --all-files
```

Expected: lychee passes. If it flags the spec link, check the relative path — this file sits in `docs/`, the spec in `docs/superpowers/specs/`.

- [ ] **Step 6: Commit**

```sh
git add docs/j2534-throughput-bench-notes.md
git commit -m "docs: record J2534 throughput baseline"
```

---

### Task 6: `SerialByteBuffer`

The core of the fix, as a pure unit with no hardware and no `QSerialPort`. Task 7 wires it in.

**Files:**
- Create: `src/platform/desktop/unix/j2534/serial_byte_buffer.h`
- Create: `src/platform/desktop/unix/j2534/serial_byte_buffer.cpp`
- Create: `src/platform/desktop/unix/j2534/serial_byte_buffer_test.cpp`
- Modify: `src/platform/desktop/unix/j2534/BUILD.bazel`

**Interfaces:**
- Consumes: nothing.
- Produces:

```cpp
class SerialByteBuffer
{
  public:
    using PollFn = std::function<QByteArray()>;
    using WaitFn = std::function<void(int ms)>;
    using NowFn = std::function<std::uint64_t()>;

    SerialByteBuffer(PollFn poll, WaitFn wait, NowFn now);

    QByteArray take(std::uint32_t n, std::uint16_t timeout_ms);
    void clear();
    std::size_t buffered() const;
};
```

Behaviour contract, matching `read_serial_data`'s original semantics exactly:

- `take` returns as soon as `n` bytes are available.
- The timeout is a **silence** timeout, not a total budget: the deadline refreshes every time any data arrives. `read_serial_data(1, 200)` today means "one byte, or 200 ms with nothing arriving", and that must not change.
- On timeout, `take` returns however many bytes it has, which may be fewer than `n` and may be zero. That is not an error — call sites already treat a short read as "no more data".
- Bytes read past `n` stay buffered for the next call. This is what makes bulk refill safe under a parser that scans for `\r\n`: over-reading never loses data.

- [ ] **Step 1: Write the failing tests**

Create `src/platform/desktop/unix/j2534/serial_byte_buffer_test.cpp`:

```cpp
#include "src/platform/desktop/unix/j2534/serial_byte_buffer.h"

#include <gtest/gtest.h>

#include <deque>
#include <vector>

namespace
{

// Drives SerialByteBuffer with scripted poll results and virtual time, so the
// silence-timeout rules are asserted deterministically rather than by sleeping.
class FakeSource
{
  public:
    void queue(QByteArray chunk)
    {
        chunks_.push_back(std::move(chunk));
    }

    SerialByteBuffer make()
    {
        return SerialByteBuffer([this] { return poll(); }, [this](int ms) { wait(ms); }, [this] { return now_; });
    }

    std::uint64_t now_ = 0;
    std::vector<int> waits;

  private:
    QByteArray poll()
    {
        if (chunks_.empty())
        {
            return {};
        }
        QByteArray chunk = std::move(chunks_.front());
        chunks_.pop_front();
        return chunk;
    }

    void wait(int ms)
    {
        waits.push_back(ms);
        now_ += static_cast<std::uint64_t>(ms);
    }

    std::deque<QByteArray> chunks_;
};

TEST(SerialByteBuffer, ServesAnExactFitFromASingleRefill)
{
    FakeSource source;
    source.queue(QByteArray("abc"));
    SerialByteBuffer buffer = source.make();

    EXPECT_EQ(buffer.take(3, 100), QByteArray("abc"));
    EXPECT_EQ(buffer.buffered(), 0u);
}

TEST(SerialByteBuffer, RetainsBytesReadPastTheRequestedCount)
{
    FakeSource source;
    source.queue(QByteArray("abcdef"));
    SerialByteBuffer buffer = source.make();

    EXPECT_EQ(buffer.take(2, 100), QByteArray("ab"));
    EXPECT_EQ(buffer.buffered(), 4u);
    // The retained bytes are served without touching the source again.
    EXPECT_EQ(buffer.take(4, 100), QByteArray("cdef"));
}

TEST(SerialByteBuffer, ServesFromTheBufferWithoutWaitingWhenItAlreadyHasEnough)
{
    FakeSource source;
    source.queue(QByteArray("abcdef"));
    SerialByteBuffer buffer = source.make();

    buffer.take(1, 100);
    source.waits.clear();
    buffer.take(1, 100);

    // This is the whole point of the class: a satisfiable one-byte read must
    // not cost an event-loop wait.
    EXPECT_TRUE(source.waits.empty());
}

TEST(SerialByteBuffer, AccumulatesAcrossSeveralRefills)
{
    FakeSource source;
    source.queue(QByteArray("ab"));
    source.queue(QByteArray("cd"));
    SerialByteBuffer buffer = source.make();

    EXPECT_EQ(buffer.take(4, 100), QByteArray("abcd"));
}

TEST(SerialByteBuffer, ReturnsWhatItHasWhenTheSourceGoesSilent)
{
    FakeSource source;
    source.queue(QByteArray("ab"));
    SerialByteBuffer buffer = source.make();

    // Asked for 5, only 2 ever arrive.
    EXPECT_EQ(buffer.take(5, 10), QByteArray("ab"));
}

TEST(SerialByteBuffer, ReturnsEmptyWhenNothingEverArrives)
{
    FakeSource source;
    SerialByteBuffer buffer = source.make();

    EXPECT_EQ(buffer.take(4, 10), QByteArray());
}

TEST(SerialByteBuffer, TheDeadlineRefreshesOnEveryArrival)
{
    FakeSource source;
    // Nine silent 1 ms waits, then a byte, repeated. With a 5 ms silence
    // timeout and no refresh this would give up long before the last byte.
    for (int i = 0; i < 3; ++i)
    {
        for (int silent = 0; silent < 4; ++silent)
        {
            source.queue(QByteArray());
        }
        source.queue(QByteArray("x"));
    }
    SerialByteBuffer buffer = source.make();

    EXPECT_EQ(buffer.take(3, 5), QByteArray("xxx"));
}

TEST(SerialByteBuffer, ZeroLengthRequestReturnsImmediately)
{
    FakeSource source;
    SerialByteBuffer buffer = source.make();

    EXPECT_EQ(buffer.take(0, 1000), QByteArray());
    EXPECT_TRUE(source.waits.empty());
}

TEST(SerialByteBuffer, ClearDiscardsRetainedBytes)
{
    FakeSource source;
    source.queue(QByteArray("abcdef"));
    SerialByteBuffer buffer = source.make();

    buffer.take(1, 100);
    ASSERT_EQ(buffer.buffered(), 5u);
    buffer.clear();

    EXPECT_EQ(buffer.buffered(), 0u);
    EXPECT_EQ(buffer.take(1, 1), QByteArray());
}

} // namespace
```

Note: `FakeSource::poll` returning an empty `QByteArray` models "nothing available yet" — that is exactly what the production poll does when `bytesAvailable()` is zero.

- [ ] **Step 2: Add the Bazel targets**

In `src/platform/desktop/unix/j2534/BUILD.bazel`, add the load for the test macro at the top, alongside the existing `qt_targets.bzl` load:

```python
load("//bazel:gtest_targets.bzl", "fastecu_gtest")
```

Then add the two targets:

```python
qt_cc_library(
    name = "serial_byte_buffer",
    srcs = ["serial_byte_buffer.cpp"],
    hdrs = ["serial_byte_buffer.h"],
    copts = COMMON_COPTS,
    target_compatible_with = select({
        "@platforms//os:windows": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    deps = QT_DEPS,
)

fastecu_gtest(
    name = "serial_byte_buffer_test",
    srcs = ["serial_byte_buffer_test.cpp"],
    target_compatible_with = select({
        "@platforms//os:windows": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    deps = [":serial_byte_buffer"],
)
```

`fastecu_gtest` (not `fastecu_portable_gtest`): this target uses `QByteArray`, so its closure links Qt by design. The unix J2534 package is not a portable root and is not listed in `//:portable_closure`.

Add `":serial_byte_buffer"` to the existing `j2534` target's `deps`, after `QT_DEPS`:

```python
    deps = QT_DEPS + [":serial_byte_buffer"],
```

No visibility change is needed: the new targets live in the same package as `:j2534`, and the package's `default_visibility` already covers the rest.

- [ ] **Step 3: Run the tests to verify they fail**

```sh
bazel test --config=release //src/platform/desktop/unix/j2534:serial_byte_buffer_test
```

Expected: FAIL — `serial_byte_buffer.h` does not exist.

- [ ] **Step 4: Write the header**

Create `src/platform/desktop/unix/j2534/serial_byte_buffer.h`:

```cpp
#pragma once

#include <QByteArray>

#include <cstddef>
#include <cstdint>
#include <functional>

// Staging buffer between the Openport's byte-oriented Tactrix protocol parser
// and the serial port.
//
// PassThruReadMsgs scans for '\r' / '\n' / ' ' terminators one byte at a time
// and genuinely cannot be batched -- it does not know a message's length until
// it has parsed the header. That parser is left exactly as written; what
// changes is the cost of a one-byte read. Previously each one cost a
// QSerialPort::read(1) plus a waitForReadyRead(1) that blocked its full
// millisecond whenever Qt had already drained the kernel buffer, so receiving
// ~200 bytes cost ~200 ms regardless of line rate. Here, one bulk refill
// serves many single-byte reads from memory.
//
// Bytes read past what a call asked for are retained, which is what makes bulk
// refill safe under a terminator-scanning parser: over-reading never loses
// data.
class SerialByteBuffer
{
  public:
    // Returns whatever is available right now, without blocking. Empty means
    // "nothing yet" and is not an error.
    using PollFn = std::function<QByteArray()>;
    // Blocks up to `ms` waiting for new data. Any return value is ignored --
    // the next poll decides what actually landed.
    using WaitFn = std::function<void(int ms)>;
    // Monotonic milliseconds. Injected so the timeout rules are testable
    // without sleeping.
    using NowFn = std::function<std::uint64_t()>;

    SerialByteBuffer(PollFn poll, WaitFn wait, NowFn now);

    // Returns up to `n` bytes, blocking until `n` are available or
    // `timeout_ms` passes with nothing arriving.
    //
    // The timeout is a SILENCE timeout, not a total budget: the deadline
    // refreshes on every arrival. This is deliberate and load-bearing --
    // read_serial_data(1, 200) has always meant "one byte, or 200 ms of
    // nothing", and changing it would change how long the app waits on a slow
    // adapter.
    //
    // A short return (including empty) is a normal timeout, not an error.
    QByteArray take(std::uint32_t n, std::uint16_t timeout_ms);

    // Drops retained bytes. Call across an open/close so stale bytes from a
    // previous connection cannot be parsed as a new message.
    void clear();

    std::size_t buffered() const;

  private:
    PollFn poll_;
    WaitFn wait_;
    NowFn now_;
    QByteArray buffer_;
};
```

- [ ] **Step 5: Write the implementation**

Create `src/platform/desktop/unix/j2534/serial_byte_buffer.cpp`:

```cpp
#include "src/platform/desktop/unix/j2534/serial_byte_buffer.h"

#include <algorithm>
#include <utility>

SerialByteBuffer::SerialByteBuffer(PollFn poll, WaitFn wait, NowFn now)
    : poll_(std::move(poll)), wait_(std::move(wait)), now_(std::move(now))
{
}

QByteArray SerialByteBuffer::take(std::uint32_t n, std::uint16_t timeout_ms)
{
    std::uint64_t deadline = now_() + timeout_ms;
    while (buffer_.size() < static_cast<qsizetype>(n))
    {
        const QByteArray chunk = poll_();
        if (!chunk.isEmpty())
        {
            buffer_.append(chunk);
            // Silence timeout: any arrival buys another full window.
            deadline = now_() + timeout_ms;
            // Re-check the size before waiting again -- the chunk may already
            // have satisfied the request.
            continue;
        }
        if (now_() >= deadline)
        {
            break;
        }
        wait_(1);
    }

    const auto wanted = std::min<qsizetype>(static_cast<qsizetype>(n), buffer_.size());
    QByteArray out = buffer_.left(wanted);
    buffer_.remove(0, wanted);
    return out;
}

void SerialByteBuffer::clear()
{
    buffer_.clear();
}

std::size_t SerialByteBuffer::buffered() const
{
    return static_cast<std::size_t>(buffer_.size());
}
```

- [ ] **Step 6: Run the tests to verify they pass**

```sh
bazel test --config=release //src/platform/desktop/unix/j2534:serial_byte_buffer_test
```

Expected: PASS, all ten tests.

- [ ] **Step 7: Commit**

```sh
git add src/platform/desktop/unix/j2534/serial_byte_buffer.h \
        src/platform/desktop/unix/j2534/serial_byte_buffer.cpp \
        src/platform/desktop/unix/j2534/serial_byte_buffer_test.cpp \
        src/platform/desktop/unix/j2534/BUILD.bazel
git commit -m "feat(j2534): add SerialByteBuffer staging buffer"
```

---

### Task 7: Buffered read and batched write in `J2534`

**Files:**
- Modify: `src/platform/desktop/unix/j2534/J2534_unix.h:9-15` (includes), `:29-31` (constructor), `:96-101` (members)
- Modify: `src/platform/desktop/unix/j2534/J2534_unix.cpp:9-15` (constructor), `:17-79` (open/close), `:81-124` (`read_serial_data`, `write_serial_data`)
- Test: `tests/tst_serial_port_crash.cpp` and the existing pty suites cover this end to end; no new test file.

**Interfaces:**
- Consumes: `SerialByteBuffer` (Task 6).
- Produces: no signature changes. `J2534::read_serial_data(std::uint32_t, std::uint16_t)` and `J2534::write_serial_data(const QByteArray&)` keep their exact signatures and return conventions, so all ~40 call sites in `PassThruReadMsgs` and friends are untouched.

- [ ] **Step 1: Add the member and include**

In `src/platform/desktop/unix/j2534/J2534_unix.h`, add to the include block:

```cpp
#include "src/platform/desktop/unix/j2534/serial_byte_buffer.h"
```

The `serial` member is `protected` (tests subclass `J2534` to drive it into the torn-down `serial == nullptr` state). Add the buffer as a **private** member, at the end of the existing private block that holds `periodic_msg_id`, `msg_ack`, `is_tx_done`:

```cpp
    SerialByteBuffer rx_buffer_;
```

`SerialByteBuffer` has no default constructor, so it must be initialised in `J2534`'s constructor initialiser list — the compiler will enforce this.

- [ ] **Step 2: Initialise the buffer**

In `src/platform/desktop/unix/j2534/J2534_unix.cpp`, replace the empty constructor:

```cpp
J2534::J2534()
    : rx_buffer_(
          [this]
          {
              if (!is_serial_port_open())
              {
                  return QByteArray{};
              }
              const qint64 available = serial->bytesAvailable();
              return available > 0 ? serial->read(available) : QByteArray{};
          },
          [this](int ms)
          {
              if (is_serial_port_open())
              {
                  serial->waitForReadyRead(ms);
              }
          },
          []
          {
              return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now().time_since_epoch())
                                                    .count());
          })
{
}
```

Add `#include <chrono>` to the file's include block.

Note the `is_serial_port_open()` guard inside both callables: the port can be torn down while a read is in flight, which is the crash `tst_serial_port_crash.cpp` exists to catch.

- [ ] **Step 3: Replace `read_serial_data`**

Replace the whole body of `J2534::read_serial_data`:

```cpp
QByteArray J2534::read_serial_data(std::uint32_t datalen, std::uint16_t timeout)
{
    return rx_buffer_.take(datalen, timeout);
}
```

The `is_serial_port_open()` check the old body performed now lives in the poll and wait callables, so a closed port yields an empty result exactly as before.

- [ ] **Step 4: Replace `write_serial_data`**

Replace the whole body of `J2534::write_serial_data`:

```cpp
int J2534::write_serial_data(const QByteArray& output)
{
    if (!is_serial_port_open())
    {
        return 1;
    }
    // One write, then an explicit flush. The previous per-byte loop left the
    // data sitting in Qt's write buffer until some later waitForReadyRead
    // happened to pump the event loop, so transmission was both fragmented
    // and timed by an unrelated call.
    if (serial->write(output) != output.size())
    {
        return 1;
    }
    serial->waitForBytesWritten(serial_read_short_timeout);
    return STATUS_NOERROR;
}
```

This does change one behaviour: a short write now returns failure where the old loop ignored `write`'s return entirely. That is the correct reading of the return value, and every caller already treats non-zero as failure.

- [ ] **Step 5: Clear the buffer across open and close**

Stale bytes from a previous connection must not be parsed as a new message.

In `open_serial_port`, immediately after the existing `serial->flush();`:

```cpp
                rx_buffer_.clear();
```

In `close_serial_port`, immediately after `serial->close();`:

```cpp
        rx_buffer_.clear();
```

- [ ] **Step 6: Check the ISO-15765 status-flag branch for a swallowed frame**

The spec names this as a risk. `SerialPortActionsDirect::read_j2534_data` (`src/platform/desktop/common/serial/serial_port_actions_direct.cpp:1194-1240`) has two branches. `is_can_connection` is **false** for an ISO-15765 connection (`DesktopCanFlashTransport::configure` sets `set_is_iso15765_connection(true)` and `set_is_can_connection(false)`), so CAN flash traffic takes the `else` branch — the one that issues an **additional** blocking `PassThruReadMsgs` at the full timeout when `RxStatus` carries `TX_DONE` or `START_OF_MESSAGE`.

Read that branch and write down, in the commit message, whether either extra read can consume a message the caller needed. Specifically: after a `TX_DONE`, the code re-reads into the same `rxmsg` and then falls through to also test `START_OF_MESSAGE` on the *new* status — confirm whether that second test is intended to apply to the re-read message or to the original.

Do not change this branch in this task. If the answer is "yes, a frame can be swallowed", that is a finding to report, not a fix to smuggle into a performance commit — it changes what the caller receives, and it needs its own test.

- [ ] **Step 7: Build and run the full suite**

```sh
bazel build --config=release //apps/bench:fastecu-bench
bazel test --config=release //...
```

Expected: green. The pty-backed suites (`tests/tst_serial_port_crash.cpp`, `tests/tst_mut_dma_integration.cpp`, and `src/platform/desktop/common/serial/direct_backend_pty_test.cpp`) exercise `PassThruReadMsgs` end to end against `MockOpenPort` and are the regression net for the parser's byte sequence. If any of them fails, the buffering has changed observable behaviour — **stop and diagnose**, do not adjust the test.

- [ ] **Step 8: Lint**

```sh
prek run --all-files
```

Expected: clean.

- [ ] **Step 9: Commit**

```sh
git add src/platform/desktop/unix/j2534/J2534_unix.h src/platform/desktop/unix/j2534/J2534_unix.cpp
git commit -m "perf(j2534): buffer serial reads and batch serial writes"
```

---

### Task 8: Measure the effect

No source changes. This is the task that decides whether the plan worked.

**Files:**
- Modify: `docs/j2534-throughput-bench-notes.md`

**Interfaces:**
- Consumes: everything through Task 7.
- Produces: a recorded post-change throughput figure, and a decision on whether Task 10 is worth doing.

- [ ] **Step 1: Verify correctness before speed**

```sh
bazel run --config=release //apps/bench:fastecu-bench -- \
  --vendor-ext --json read 0x8056a8 256
```

Compare the `data` field against the same read recorded during Task 5's session. **The bytes must be identical.** A faster read that returns different bytes is a broken read — stop and diagnose.

- [ ] **Step 2: Re-run the baseline command three times**

```sh
bazel run --config=release //apps/bench:fastecu-bench -- \
  --vendor-ext --stats --json read 0x8056a8 8192
```

Record all three JSON lines.

- [ ] **Step 3: Append the results**

Add to `docs/j2534-throughput-bench-notes.md`:

```markdown
## After buffered serial I/O

Commit: FILL IN

| Run | ms | exchanges | bytes/s | ms/exchange |
| --- | --- | --- | --- | --- |
| 1 | | | | |
| 2 | | | | |
| 3 | | | | |

Raw JSON:

```
FILL IN
```

Speedup over baseline: FILL IN×

Data correctness: the 256-byte read at 0x8056a8 returned bytes identical to
the baseline session's.
```

- [ ] **Step 4: Judge the result against the spec's target**

The spec's target is within roughly 2× of CAN wire time — about 7.6 ms per 192-byte chunk, so **~15 ms/exchange or better**.

Write one short paragraph in the notes file stating whether the target was met, using the measured `ms/exchange`. Do not round in the plan's favour.

- [ ] **Step 5: Commit**

```sh
git add docs/j2534-throughput-bench-notes.md
git commit -m "docs: record throughput after buffered serial I/O"
```

---

### Task 9: Make `SET_CONFIG` failures observable

`PassThruIoctl` under `SET_CONFIG` writes `ats<chan> <param> <value>`, reads the adapter's reply, logs it, and discards it — returning `STATUS_NOERROR` on every path (`J2534_unix.cpp:1029-1061`). This is why PR #176's fallback was unreachable code and its success unverifiable.

**Files:**
- Modify: `src/platform/desktop/unix/j2534/J2534_unix.cpp:1029-1061` (`PassThruIoctl`, `SET_CONFIG` branch)
- Audit (may not need changes): `src/platform/desktop/common/serial/serial_port_actions_direct.cpp` lines 107, 1256, 1527, 1694, 1721

**Interfaces:**
- Consumes: `read_serial_data` from Task 7.
- Produces: `PassThruIoctl(chan, SET_CONFIG, ...)` returns `ERR_FAILED` when the adapter does not acknowledge a parameter.

Adapter replies, per `src/platform/desktop/unix/j2534/testing/mock_openport.h`: `aro\r\n` acknowledges, `are ...\r\n` reports an error.

- [ ] **Step 1: Audit every caller before changing anything**

Read each of the five call sites listed above and write down, for each, what it does with a non-zero return. Two of them (line 107 and line 1527) sit on paths that run for **K-Line** connections as well as CAN, so a newly-propagated failure there changes K-Line behaviour.

Record the audit as a comment block in the commit message for this task. If any caller would now abort a previously-working K-Line connection, either fix that caller or leave the parameter it sets exempt with an inline comment saying why. Do not silently widen the change.

- [ ] **Step 2: Replace the fixed-length reply read with a line read**

In the `SET_CONFIG` branch of `PassThruIoctl`, replace:

```cpp
            write_serial_data(output);
            emit LOG_D("Sent: " + parseMessageToHex(output), true, true);
            received = read_serial_data(100, 50);
            emit LOG_D("Response: " + parseMessageToHex(received), true, true);
```

with:

```cpp
            write_serial_data(output);
            emit LOG_D("Sent: " + parseMessageToHex(output), true, true);
            // Read exactly one reply line. The previous fixed-length
            // read_serial_data(100, 50) could never reach 100 bytes, so it
            // always paid its full 50 ms silence timeout per parameter.
            received.clear();
            while (!received.endsWith('\n'))
            {
                const QByteArray chunk = read_serial_data(1, serial_read_extra_short_timeout);
                if (chunk.isEmpty())
                {
                    break;
                }
                received.append(chunk);
            }
            emit LOG_D("Response: " + parseMessageToHex(received), true, true);
            if (!received.startsWith("aro"))
            {
                emit LOG_E(QString("Adapter rejected SET_CONFIG parameter %1 (value %2): %3")
                               .arg(cfgitem_local->Parameter)
                               .arg(cfgitem_local->Value)
                               .arg(QString::fromUtf8(received.trimmed())),
                           true, true);
                return ERR_FAILED;
            }
```

The parameter is logged numerically on purpose. `dump_sconfig_param` already owns a switch mapping the number to a name, but it writes into a local and returns nothing; hoisting that switch into a shared helper is a refactor this task does not need. The number plus the adapter's own error text is enough to identify the rejected parameter, and `dump_sconfig_param` has already logged its name for the same call.

- [ ] **Step 3: Build and run the full suite**

```sh
bazel test --config=release //...
```

Expected: green. `MockOpenPort` replies `aro\r\n` to every `at*` command other than `ata`/`ati`, so the pty suites exercise the acknowledged path.

- [ ] **Step 4: Verify against hardware that a normal connection still works**

```sh
bazel run --config=release //apps/bench:fastecu-bench -- --vendor-ext --json read 0x8056a8 256
```

Expected: `ok`, with the same bytes as Task 8 Step 1. A `SET_CONFIG` that the real adapter rejects will now fail the connection loudly — if that happens, the audit in Step 1 missed a parameter this adapter does not support, and that is the finding, not a reason to revert the error propagation.

- [ ] **Step 5: Commit**

```sh
git add src/platform/desktop/unix/j2534/J2534_unix.cpp
git commit -m "fix(j2534): propagate SET_CONFIG rejections instead of swallowing them"
```

Include the Step 1 audit in the commit message body.

---

### Task 10: Re-land PR #176's ISO-TP flow control, now measurable

With Task 9 in place, `SET_CONFIG` failures are real, so #176's fallback becomes exercised code rather than an assertion nobody could check.

**Files:**
- Create: `src/platform/desktop/common/serial/j2534_can_timing_config.h`
- Create: `src/platform/desktop/common/serial/j2534_can_timing_config.cpp`
- Create: `src/platform/desktop/common/serial/j2534_can_timing_config_test.cpp`
- Modify: `src/platform/desktop/common/serial/serial_port_actions_direct.cpp:1512-1535` (`set_j2534_can_timings`), `:1417-1425` (`init_j2534_connection`)
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`
- Modify: `docs/j2534-throughput-bench-notes.md`

**Interfaces:**
- Consumes: `PassThruIoctl`'s real return value (Task 9).
- Produces: `bool configureJ2534CanTimings(bool iso15765, const J2534SetConfig& setConfig)`, where `using J2534SetConfig = std::function<long(const SCONFIG_LIST&)>;`.

- [ ] **Step 1: Recover the original change**

The three files and both edits already exist on the unmerged PR #176 branch. Recover them rather than retyping:

```sh
git show 2bf1a395db063deb5c2b520cbb722a66ec455ef0 --stat
git cherry-pick -n 2bf1a395db063deb5c2b520cbb722a66ec455ef0
```

If the cherry-pick conflicts, resolve in favour of the current `master` structure and keep #176's logic. Do not cherry-pick the two doc commits (`e4ba4597`, `7a12c819`) — this plan's design doc supersedes them.

- [ ] **Step 2: Run the recovered tests**

```sh
bazel test --config=release //src/platform/desktop/common/serial:all
```

Expected: PASS. `j2534_can_timing_config_test.cpp` covers initial settings, the fallback, terminal failure, and raw-CAN isolation.

- [ ] **Step 3: Run the full suite**

```sh
bazel test --config=release //...
prek run --all-files
```

Expected: green.

- [ ] **Step 4: Measure**

```sh
bazel run --config=release //apps/bench:fastecu-bench -- \
  --vendor-ext --stats --json read 0x8056a8 8192
```

Three runs, recorded.

- [ ] **Step 5: Append the results and decide**

Add to `docs/j2534-throughput-bench-notes.md`:

```markdown
## After ISO-TP flow-control tuning (STMIN=0, BS=0)

Commit: FILL IN

| Run | ms | exchanges | bytes/s | ms/exchange |
| --- | --- | --- | --- | --- |
| 1 | | | | |
| 2 | | | | |
| 3 | | | | |

Change relative to buffered serial I/O alone: FILL IN

Which STMIN/BS values the adapter actually accepted: FILL IN — state whether
the initial (0, 0) setting was acknowledged or whether the (1, 16) fallback
was used. This is now observable because SET_CONFIG propagates rejections.
```

**If the measured change is within run-to-run noise, say so plainly and keep the change anyway** — the value delivered here is that the setting is now verifiable, not that it was necessarily fast. Do not manufacture a speedup narrative.

- [ ] **Step 6: Commit**

```sh
git add src/platform/desktop/common/serial/ docs/j2534-throughput-bench-notes.md
git commit -m "perf(j2534): tune ISO15765 flow control with a verifiable fallback"
```

---

### Task 11: Close out

**Files:**
- Modify: `docs/superpowers/specs/2026-08-23-j2534-serial-throughput-design.md` (status line)
- Modify: `docs/j2534-throughput-bench-notes.md` (summary)

- [ ] **Step 1: Check every success criterion in the spec**

The spec lists four. Verify each against real evidence, not memory:

1. `bazel test --config=release //...` green and `prek run --all-files` clean — run both now.
2. `--vendor-ext` establishes a session and a bulk read returns correct data — Task 8 Step 1.
3. Post-change throughput within ~2× of CAN wire time, baseline and result both recorded — Task 8 Step 4.
4. `SET_CONFIG` returns a failure status on rejection, demonstrated by a test — Task 10 Step 2.

If any criterion is unmet, write down which and why. Do not mark the spec complete over an unmet criterion.

- [ ] **Step 2: Update the spec status**

Change the status line from `Status: approved, not yet implemented` to `Status: implemented`, followed by a one-line markdown link to the bench notes created in Task 5. From the spec's location that link is `[bench measurements](../../j2534-throughput-bench-notes.md)` — write it as a real link, not a backticked path, since lychee checks links and cannot see a path written as inline code. If a criterion was unmet, say `Status: implemented, with exceptions` and name them.

- [ ] **Step 3: Add a summary to the notes file**

One short table at the top of `docs/j2534-throughput-bench-notes.md`: baseline bytes/s, after buffering, after flow-control tuning, and the CAN wire-time reference. Measured numbers only.

- [ ] **Step 4: Commit and push**

```sh
git add docs/
git commit -m "docs: close out J2534 throughput work"
git push -u origin perf/j2534-serial-throughput
```

- [ ] **Step 5: Open the pull request**

```sh
gh pr create --base master --title "perf: fix J2534 serial throughput on Unix" --body "$(cat <<'EOF'
### What

- Buffers Unix J2534 serial reads and batches writes, replacing per-byte I/O
  that cost roughly 1 ms per received byte regardless of line rate.
- Adds `--vendor-ext` to the bench CLI so ROMs carrying the third-party vendor
  diagnostic extension can be reached at all.
- Adds `--stats` derived throughput figures.
- Propagates `SET_CONFIG` rejections instead of returning STATUS_NOERROR
  unconditionally.
- Re-lands the ISO-TP flow-control tuning from #176 on top of that, where its
  fallback is now reachable code.

### Why

FILL IN — the measured before/after from docs/j2534-throughput-bench-notes.md.

### References

- Design: `docs/superpowers/specs/2026-08-23-j2534-serial-throughput-design.md`
- Plan: `docs/superpowers/plans/2026-08-23-j2534-serial-throughput.md`
- Measurements: `docs/j2534-throughput-bench-notes.md`
- Supersedes #176

### Verification

FILL IN — `bazel test --config=release //...` result, `prek run --all-files`,
and the hardware runs.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01LuvAHBB3aBNX4kwuuWPcFv
EOF
)"
```

Replace both `FILL IN` blocks with real results before creating the PR.

- [ ] **Step 6: Note that #176 is superseded**

Add a comment on PR #176 pointing at the new PR and summarising why the original approach could not have worked — `PassThruIoctl(SET_CONFIG)` discarded the adapter's reply, so the fallback was unreachable and the tuning unverifiable, and the real bottleneck was host-side per-byte serial I/O. Do not close #176 without the user asking.
